#include "shape_modifiers.h"

#include <algorithm>
#include <cmath>

#include "noise.h"
#include "xt/geom.h"
#include "xt/num_guard.h"

/// See shape_modifiers.h and docs/design/31-vector-authoring.md §31.6.1 (T-225).

namespace xt::modifiers {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kKappa = 0.5523f;           // Rounded-corner handle ratio (§31.6.1 round, star).
constexpr float kArcKappa = 0.5522847498f;  // Circular-arc constant (rect corners, ellipse).
constexpr float kEps = 1e-4f;

float lengthOf(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

/// A finite count parameter rounded and clamped to [0, kMaxGeneratedPoints] (T-258a).
int generatedCount(float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, static_cast<float>(kMaxGeneratedPoints))));
}

/// Star / polygon `points`: generatedCount(), 0 (a degenerate star) when non-finite.
int starPoints(float v) { return std::isfinite(v) ? generatedCount(v) : 0; }

/// Appends the input path unchanged (a modifier whose parameter is not finite does nothing).
void copyPath(std::span<const PathVerb> verbs, std::span<const float> points, std::vector<PathVerb>& outVerbs,
              std::vector<float>& outPoints) {
    outVerbs.insert(outVerbs.end(), verbs.begin(), verbs.end());
    outPoints.insert(outPoints.end(), points.begin(), points.end());
}

Vec2 lerp(Vec2 a, Vec2 b, float t) { return a + (b - a) * t; }

Vec2 normalized(Vec2 v) {
    const float len = lengthOf(v);
    return len > 1e-12f ? v / len : Vec2{0, 0};
}

Vec2 rotateDeg(Vec2 v, float deg) {
    const float r = deg * kPi / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);
    return Vec2{v.x * c - v.y * s, v.x * s + v.y * c};
}

// --- Contour model: vertices with absolute in/out handles (a zero handle equals its vertex). ---

struct Vtx {
    Vec2 in;
    Vec2 p;
    Vec2 out;
};

struct Contour {
    std::vector<Vtx> v;
    bool closed = false;
};

/// Splits raw verbs/points into contours. A Close whose last vertex repeats the first merges the
/// two (the first vertex takes the last one's in-handle), so every closed contour lists each
/// corner once.
void readContours(std::span<const PathVerb> verbs, std::span<const float> points, std::vector<Contour>& out) {
    size_t pc = 0;
    auto pt = [&](size_t i) { return Vec2{points[i], points[i + 1]}; };
    auto startIfNeeded = [&](Vec2 at) {
        if (out.empty() || out.back().closed) {
            Vec2 start = out.empty() || out.back().v.empty() ? at : out.back().v.front().p;
            out.push_back(Contour{});
            out.back().v.push_back(Vtx{start, start, start});
        }
    };
    for (PathVerb verb : verbs) {
        switch (verb) {
            case PathVerb::Move: {
                const Vec2 p = pt(pc);
                pc += 2;
                out.push_back(Contour{});
                out.back().v.push_back(Vtx{p, p, p});
                break;
            }
            case PathVerb::Line: {
                const Vec2 p = pt(pc);
                pc += 2;
                startIfNeeded(Vec2{0, 0});
                out.back().v.push_back(Vtx{p, p, p});
                break;
            }
            case PathVerb::Cubic: {
                const Vec2 c1 = pt(pc);
                const Vec2 c2 = pt(pc + 2);
                const Vec2 p = pt(pc + 4);
                pc += 6;
                startIfNeeded(Vec2{0, 0});
                out.back().v.back().out = c1;
                out.back().v.push_back(Vtx{c2, p, p});
                break;
            }
            case PathVerb::Close: {
                if (out.empty() || out.back().closed) {
                    break;
                }
                Contour& c = out.back();
                c.closed = true;
                if (c.v.size() >= 2 && lengthOf(c.v.back().p - c.v.front().p) < kEps) {
                    c.v.front().in = c.v.back().in;
                    c.v.pop_back();
                }
                break;
            }
        }
    }
}

void pushPoint(std::vector<float>& pts, Vec2 p) {
    pts.push_back(p.x);
    pts.push_back(p.y);
}

bool straight(const Vtx& a, const Vtx& b) { return a.out == a.p && b.in == b.p; }

void writeSegment(const Vtx& a, const Vtx& b, std::vector<PathVerb>& verbs, std::vector<float>& pts) {
    if (straight(a, b)) {
        verbs.push_back(PathVerb::Line);
        pushPoint(pts, b.p);
    } else {
        verbs.push_back(PathVerb::Cubic);
        pushPoint(pts, a.out);
        pushPoint(pts, b.in);
        pushPoint(pts, b.p);
    }
}

/// Writes contours back as verbs/points: straight segments as Line, others as Cubic; a closed
/// contour's closing segment is a Close alone when straight, else a Cubic followed by Close.
void writeContour(const Contour& c, std::vector<PathVerb>& verbs, std::vector<float>& pts) {
    if (c.v.empty()) {
        return;
    }
    verbs.push_back(PathVerb::Move);
    pushPoint(pts, c.v.front().p);
    for (size_t i = 0; i + 1 < c.v.size(); ++i) {
        writeSegment(c.v[i], c.v[i + 1], verbs, pts);
    }
    if (c.closed) {
        const Vtx& last = c.v.back();
        const Vtx& first = c.v.front();
        if (c.v.size() >= 2 && !straight(last, first)) {
            writeSegment(last, first, verbs, pts);
        } else if (c.v.size() == 1 && !(last.out == last.p && first.in == first.p)) {
            writeSegment(last, first, verbs, pts);
        }
        verbs.push_back(PathVerb::Close);
    }
}

// --- One segment of a contour as a cubic (a line keeps its handles on its end points). ---

struct Seg {
    Vec2 p0, c1, c2, p3;
    bool line = true;
};

Seg segBetween(const Vtx& a, const Vtx& b) {
    Seg s;
    s.p0 = a.p;
    s.c1 = a.out;
    s.c2 = b.in;
    s.p3 = b.p;
    s.line = straight(a, b);
    return s;
}

size_t segCount(const Contour& c) {
    if (c.v.size() < 2) {
        return 0;
    }
    return c.closed ? c.v.size() : c.v.size() - 1;
}

Seg segAt(const Contour& c, size_t j) { return segBetween(c.v[j], c.v[(j + 1) % c.v.size()]); }

Vec2 cubicPoint(const Seg& s, float t) {
    const float u = 1.0f - t;
    return s.p0 * (u * u * u) + s.c1 * (3 * u * u * t) + s.c2 * (3 * u * t * t) + s.p3 * (t * t * t);
}

Vec2 cubicDerivative(const Seg& s, float t) {
    const float u = 1.0f - t;
    return (s.c1 - s.p0) * (3 * u * u) + (s.c2 - s.c1) * (6 * u * t) + (s.p3 - s.c2) * (3 * t * t);
}

float segLength(const Seg& s) {
    if (s.line) {
        return lengthOf(s.p3 - s.p0);
    }
    return geom::buildCubicTable(s.p0, s.c1, s.c2, s.p3).length;
}

/// Bezier parameter at arc-length fraction `frac` (identity for a line).
float segParam(const Seg& s, float frac) {
    if (s.line || frac <= 0.0f || frac >= 1.0f) {
        return std::clamp(frac, 0.0f, 1.0f);
    }
    return geom::lengthFractionToT(geom::buildCubicTable(s.p0, s.c1, s.c2, s.p3), frac);
}

Vec2 segPointAtFrac(const Seg& s, float frac) {
    return s.line ? lerp(s.p0, s.p3, frac) : cubicPoint(s, segParam(s, frac));
}

/// Unit tangent (direction of travel) at arc fraction `frac`.
Vec2 segTangentAtFrac(const Seg& s, float frac) {
    if (s.line) {
        return normalized(s.p3 - s.p0);
    }
    Vec2 d = cubicDerivative(s, segParam(s, frac));
    if (lengthOf(d) < 1e-9f) {
        d = s.p3 - s.p0;
    }
    return normalized(d);
}

/// The part of `s` between arc fractions [fa, fb].
Seg subSeg(const Seg& s, float fa, float fb) {
    Seg r;
    r.line = s.line;
    if (s.line) {
        r.p0 = lerp(s.p0, s.p3, fa);
        r.p3 = lerp(s.p0, s.p3, fb);
        r.c1 = r.p0;
        r.c2 = r.p3;
        return r;
    }
    const float ta = segParam(s, fa);
    const float tb = segParam(s, fb);
    Vec2 out[4];
    geom::subCubic(s.p0, s.c1, s.c2, s.p3, ta, tb, out);
    r.p0 = out[0];
    r.c1 = out[1];
    r.c2 = out[2];
    r.p3 = out[3];
    return r;
}

/// The part of `s` between bezier parameters [ta, tb] (lines by parameter too).
Seg subSegParam(const Seg& s, float ta, float tb) {
    Vec2 out[4];
    geom::subCubic(s.p0, s.c1, s.c2, s.p3, ta, tb, out);
    Seg r;
    r.p0 = out[0];
    r.c1 = out[1];
    r.c2 = out[2];
    r.p3 = out[3];
    r.line = false;
    return r;
}

// --- trim ---

struct Edge {
    bool cubic;
    Vec2 p0, p1, p2, p3;
    float length;
    float cumStart;
    uint32_t subpath;
};

/// Same edge list as path_trim.cpp's trimPath (a Close is a line back to the subpath start).
/// Returns the total length; `closedSingle` = exactly one subpath and it is closed.
float buildEdges(const PathSpan& path, std::vector<Edge>& edges, bool& closedSingle) {
    edges.clear();
    size_t pc = 0;
    Vec2 cur{0, 0};
    Vec2 start{0, 0};
    uint32_t sub = 0;
    uint32_t moves = 0;
    bool have = false;
    bool lastClosed = false;
    float cum = 0.0f;
    for (PathVerb verb : path.verbs) {
        switch (verb) {
            case PathVerb::Move:
                cur = Vec2{path.points[pc], path.points[pc + 1]};
                pc += 2;
                start = cur;
                if (have) {
                    ++sub;
                }
                have = true;
                ++moves;
                lastClosed = false;
                break;
            case PathVerb::Line: {
                const Vec2 e{path.points[pc], path.points[pc + 1]};
                pc += 2;
                const float len = lengthOf(e - cur);
                edges.push_back(Edge{false, cur, {}, {}, e, len, cum, sub});
                cum += len;
                cur = e;
                have = true;
                lastClosed = false;
                break;
            }
            case PathVerb::Cubic: {
                const Vec2 c1{path.points[pc], path.points[pc + 1]};
                const Vec2 c2{path.points[pc + 2], path.points[pc + 3]};
                const Vec2 e{path.points[pc + 4], path.points[pc + 5]};
                pc += 6;
                const float len = geom::buildCubicTable(cur, c1, c2, e).length;
                edges.push_back(Edge{true, cur, c1, c2, e, len, cum, sub});
                cum += len;
                cur = e;
                have = true;
                lastClosed = false;
                break;
            }
            case PathVerb::Close:
                if (have) {
                    const float len = lengthOf(start - cur);
                    edges.push_back(Edge{false, cur, {}, {}, start, len, cum, sub});
                    cum += len;
                    cur = start;
                    lastClosed = true;
                }
                break;
        }
    }
    closedSingle = moves <= 1 && lastClosed;
    return cum;
}

/// Emits the part of `edges` in [s0, s1] (length units). The first fragment starts with a Move
/// unless `continueLast`, in which case it continues the previously emitted fragment.
void emitTrimRange(const std::vector<Edge>& edges, float s0, float s1, bool continueLast,
                   std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    bool started = false;
    uint32_t lastSub = 0;
    for (const Edge& edge : edges) {
        const float edgeEnd = edge.cumStart + edge.length;
        if (edgeEnd <= s0 || edge.cumStart >= s1) {
            continue;
        }
        const float localS = s0 > edge.cumStart ? s0 - edge.cumStart : 0.0f;
        const float localE = s1 < edgeEnd ? s1 - edge.cumStart : edge.length;
        if (localE <= localS && edge.length > 0.0f) {
            continue;
        }
        const float fa = edge.length > 1e-9f ? localS / edge.length : 0.0f;
        const float fb = edge.length > 1e-9f ? localE / edge.length : 1.0f;
        const bool newFragment = started ? edge.subpath != lastSub : !continueLast;
        if (edge.cubic) {
            const geom::CubicTable table = geom::buildCubicTable(edge.p0, edge.p1, edge.p2, edge.p3);
            Vec2 sub[4];
            geom::subCubic(edge.p0, edge.p1, edge.p2, edge.p3, geom::lengthFractionToT(table, fa),
                           geom::lengthFractionToT(table, fb), sub);
            if (newFragment) {
                outVerbs.push_back(PathVerb::Move);
                pushPoint(outPoints, sub[0]);
            }
            outVerbs.push_back(PathVerb::Cubic);
            pushPoint(outPoints, sub[1]);
            pushPoint(outPoints, sub[2]);
            pushPoint(outPoints, sub[3]);
        } else {
            if (newFragment) {
                outVerbs.push_back(PathVerb::Move);
                pushPoint(outPoints, lerp(edge.p0, edge.p3, fa));
            }
            outVerbs.push_back(PathVerb::Line);
            pushPoint(outPoints, lerp(edge.p0, edge.p3, fb));
        }
        started = true;
        lastSub = edge.subpath;
    }
}

struct LocalRange {
    float s0;
    float s1;
};

/// Trims one path to `ranges` (its own length units, in order); a range starting at 0 right after
/// one ending at the path's end continues it when the path is one closed contour.
void trimOne(const std::vector<Edge>& edges, float total, bool closedSingle,
             std::span<const LocalRange> ranges, std::vector<PathVerb>& outVerbs,
             std::vector<float>& outPoints) {
    bool prevAtEnd = false;
    for (const LocalRange& r : ranges) {
        const size_t before = outVerbs.size();
        const bool join = prevAtEnd && closedSingle && r.s0 <= kEps * std::max(1.0f, total) && before > 0;
        emitTrimRange(edges, r.s0, r.s1, join, outVerbs, outPoints);
        prevAtEnd = outVerbs.size() > before && r.s1 >= total - kEps * std::max(1.0f, total);
    }
}

}  // namespace

void trimPaths(std::span<const PathSpan> paths, float start, float end, float offsetDeg, uint8_t mode,
               std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints,
               std::vector<PathRange>& outRanges) {
    float s = std::clamp(start, 0.0f, 1.0f);
    float e = std::clamp(end, 0.0f, 1.0f);
    float o = std::fmod(offsetDeg, 360.0f) / 360.0f;
    if (o < 0.0f) {
        o += 1.0f;
    }
    s += o;
    e += o;
    if (s > e) {
        std::swap(s, e);
    }
    const bool full = (e - s) >= 1.0f;
    const bool empty = e == s;
    if (s >= 1.0f) {
        s -= 1.0f;
        e -= 1.0f;
    }
    LocalRange fracs[2];
    size_t fracCount = 0;
    if (e <= 1.0f) {
        fracs[fracCount++] = LocalRange{s, e};
    } else {
        fracs[fracCount++] = LocalRange{s, 1.0f};
        fracs[fracCount++] = LocalRange{0.0f, e - 1.0f};
    }

    std::vector<Edge> edges;
    std::vector<float> totals(paths.size());
    std::vector<uint8_t> closed(paths.size());
    float grand = 0.0f;
    for (size_t i = 0; i < paths.size(); ++i) {
        bool cs = false;
        totals[i] = buildEdges(paths[i], edges, cs);
        closed[i] = cs ? 1 : 0;
        grand += totals[i];
    }

    std::vector<LocalRange> local;
    float cum = 0.0f;
    for (size_t i = 0; i < paths.size(); ++i) {
        PathRange range;
        range.verbOffset = static_cast<uint32_t>(outVerbs.size());
        range.pointOffset = static_cast<uint32_t>(outPoints.size());
        const float total = totals[i];
        if (full) {
            outVerbs.insert(outVerbs.end(), paths[i].verbs.begin(), paths[i].verbs.end());
            outPoints.insert(outPoints.end(), paths[i].points.begin(), paths[i].points.end());
        } else if (!empty && total > 0.0f) {
            local.clear();
            for (size_t k = 0; k < fracCount; ++k) {
                if (mode == kTrimSerial) {
                    const float a = std::max(fracs[k].s0 * grand, cum) - cum;
                    const float b = std::min(fracs[k].s1 * grand, cum + total) - cum;
                    if (b > a) {
                        local.push_back(LocalRange{a, b});
                    }
                } else {
                    local.push_back(LocalRange{fracs[k].s0 * total, fracs[k].s1 * total});
                }
            }
            bool cs = false;
            buildEdges(paths[i], edges, cs);
            trimOne(edges, total, closed[i] != 0, local, outVerbs, outPoints);
        }
        cum += total;
        range.verbCount = static_cast<uint32_t>(outVerbs.size()) - range.verbOffset;
        range.pointCount = static_cast<uint32_t>(outPoints.size()) - range.pointOffset;
        outRanges.push_back(range);
    }
}

void repeatCopies(const ModifierNodeData& repeat, std::vector<RepeatCopy>& out) {
    // T-258a: a non-finite count makes no copies.
    const float c =
        std::round(clampFinite(repeat.params[0], 0.0f, static_cast<float>(kMaxRepeatCopies), 0.0f));
    const uint32_t n = toUint32Sat(c);
    const float offset = repeat.params[1];
    const Vec2 anchor{repeat.params[2], repeat.params[3]};
    const Vec2 translate{repeat.params[4], repeat.params[5]};
    const float rotation = repeat.params[6];
    const float scale = std::max(repeat.params[7], 0.0f);
    for (uint32_t i = 0; i < n; ++i) {
        const float alpha = repeat.opacityA + (repeat.opacityB - repeat.opacityA) * static_cast<float>(i) /
                                                  static_cast<float>(n);
        if (alpha <= 0.0f) {
            continue;
        }
        const float t = offset + static_cast<float>(i);
        const float st = std::pow(scale, t);
        Mat2x3 m = mul(translation(translate * t + anchor),
                       mul(rotationDeg(rotation * t), mul(scaling(Vec2{st, st}), translation(-anchor))));
        out.push_back(RepeatCopy{m, alpha, i});
    }
}

void roundCorners(std::span<const PathVerb> verbs, std::span<const float> points, float radius,
                  std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    std::vector<Contour> contours;
    readContours(verbs, points, contours);
    std::vector<Seg> segs;
    std::vector<float> lens;
    std::vector<float> cutIn;   // Per vertex: arc distance cut from the segment ending there.
    std::vector<float> cutOut;  // Per vertex: arc distance cut from the segment starting there.
    for (Contour& c : contours) {
        const size_t n = c.v.size();
        const size_t sc = segCount(c);
        if (radius <= 0.0f || sc == 0) {
            writeContour(c, outVerbs, outPoints);
            continue;
        }
        segs.resize(sc);
        lens.resize(sc);
        for (size_t j = 0; j < sc; ++j) {
            segs[j] = segAt(c, j);
            lens[j] = segLength(segs[j]);
        }
        cutIn.assign(n, 0.0f);
        cutOut.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i) {
            const Vtx& v = c.v[i];
            if (!(v.in == v.p && v.out == v.p)) {
                continue;  // Not a corner: a handle is set.
            }
            if (!c.closed && (i == 0 || i + 1 == n)) {
                continue;  // Open end point.
            }
            const size_t prev = (i + sc - 1) % sc;  // Segment ending at i.
            const size_t next = i % sc;             // Segment starting at i.
            const float dIn = std::min(radius, lens[prev] * 0.5f);
            const float dOut = std::min(radius, lens[next] * 0.5f);
            if (dIn <= 0.0f || dOut <= 0.0f) {
                continue;
            }
            cutIn[i] = dIn;
            cutOut[i] = dOut;
        }
        // Trim every segment by its end vertices' cuts.
        std::vector<Seg> trimmed(sc);
        for (size_t j = 0; j < sc; ++j) {
            const size_t a = j;
            const size_t b = (j + 1) % n;
            const float fa = lens[j] > 0.0f ? cutOut[a] / lens[j] : 0.0f;
            const float fb = lens[j] > 0.0f ? 1.0f - cutIn[b] / lens[j] : 1.0f;
            trimmed[j] = (fa > 0.0f || fb < 1.0f) ? subSeg(segs[j], fa, fb) : segs[j];
        }
        Contour r;
        r.closed = c.closed;
        for (size_t i = 0; i < n; ++i) {
            const bool hasPrev = c.closed || i > 0;
            const bool hasNext = c.closed || i + 1 < n;
            const Seg* sp = hasPrev ? &trimmed[(i + sc - 1) % sc] : nullptr;
            const Seg* sn = hasNext ? &trimmed[i % sc] : nullptr;
            if (cutIn[i] > 0.0f && sp != nullptr && sn != nullptr) {
                const Vec2 a = sp->p3;
                const Vec2 b = sn->p0;
                Vec2 dirA = normalized(sp->p3 - sp->c2);
                if (lengthOf(dirA) == 0.0f) {
                    dirA = normalized(sp->p3 - sp->p0);
                }
                Vec2 dirB = normalized(sn->c1 - sn->p0);
                if (lengthOf(dirB) == 0.0f) {
                    dirB = normalized(sn->p3 - sn->p0);
                }
                r.v.push_back(Vtx{sp->c2, a, a + dirA * (kKappa * cutIn[i])});
                r.v.push_back(Vtx{b - dirB * (kKappa * cutOut[i]), b, sn->c1});
            } else {
                const Vec2 p = sn != nullptr ? sn->p0 : sp->p3;
                r.v.push_back(Vtx{sp != nullptr ? sp->c2 : p, p, sn != nullptr ? sn->c1 : p});
            }
        }
        writeContour(r, outVerbs, outPoints);
    }
}

void pucker(std::span<const PathVerb> verbs, std::span<const float> points, float amount,
            std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    std::vector<Contour> contours;
    readContours(verbs, points, contours);
    const float a = std::clamp(amount / 100.0f, -1.0f, 1.0f);
    for (Contour& c : contours) {
        if (a != 0.0f && !c.v.empty()) {
            Vec2 center{0, 0};
            for (const Vtx& v : c.v) {
                center += v.p;
            }
            center = center / static_cast<float>(c.v.size());
            for (Vtx& v : c.v) {
                v.p = lerp(v.p, center, a);
                v.in = lerp(v.in, center, -a);
                v.out = lerp(v.out, center, -a);
            }
        }
        writeContour(c, outVerbs, outPoints);
    }
}

void zigzag(std::span<const PathVerb> verbs, std::span<const float> points, float size, float ridges,
            bool smooth, std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    if (!std::isfinite(ridges)) {
        copyPath(verbs, points, outVerbs, outPoints);  // T-258a
        return;
    }
    std::vector<Contour> contours;
    readContours(verbs, points, contours);
    const int freq = generatedCount(ridges);
    std::vector<Vec2> pos;
    std::vector<Vec2> tan;
    std::vector<int> segOf;     // Segment an inserted point lies on; -1 for an original vertex.
    std::vector<int> vertexOf;  // Original vertex index; -1 for an inserted point.
    for (Contour& c : contours) {
        const size_t n = c.v.size();
        const size_t sc = segCount(c);
        if (sc == 0) {
            writeContour(c, outVerbs, outPoints);
            continue;
        }
        std::vector<Seg> segs(sc);
        std::vector<float> handleLen(sc);
        for (size_t j = 0; j < sc; ++j) {
            segs[j] = segAt(c, j);
            handleLen[j] = lengthOf(segs[j].p3 - segs[j].p0) / (2.0f * static_cast<float>(freq + 1));
        }
        pos.clear();
        tan.clear();
        segOf.clear();
        vertexOf.clear();
        for (size_t j = 0; j < sc; ++j) {
            pos.push_back(c.v[j].p);
            tan.push_back(Vec2{});
            segOf.push_back(-1);
            vertexOf.push_back(static_cast<int>(j));
            for (int k = 0; k < freq; ++k) {
                const float f = static_cast<float>(k + 1) / static_cast<float>(freq + 1);
                pos.push_back(segPointAtFrac(segs[j], f));
                tan.push_back(segTangentAtFrac(segs[j], f));
                segOf.push_back(static_cast<int>(j));
                vertexOf.push_back(-1);
            }
        }
        if (!c.closed) {
            pos.push_back(c.v[n - 1].p);
            tan.push_back(Vec2{});
            segOf.push_back(-1);
            vertexOf.push_back(static_cast<int>(n - 1));
        }
        const size_t count = pos.size();
        // Original vertices: tangent along the prev -> next chord (open ends one-sided).
        for (size_t i = 0; i < count; ++i) {
            if (vertexOf[i] < 0) {
                continue;
            }
            const bool hasPrev = c.closed || i > 0;
            const bool hasNext = c.closed || i + 1 < count;
            const Vec2 prev = hasPrev ? pos[(i + count - 1) % count] : pos[i];
            const Vec2 next = hasNext ? pos[(i + 1) % count] : pos[i];
            tan[i] = normalized(next - prev);
        }
        Contour r;
        r.closed = c.closed;
        for (size_t i = 0; i < count; ++i) {
            float sign = (i % 2 == 0) ? 1.0f : -1.0f;
            if (c.closed && (count % 2) == 1 && i + 1 == count) {
                sign = 0.0f;
            }
            const Vec2 normal{-tan[i].y, tan[i].x};
            const Vec2 q = pos[i] + normal * (sign * size);
            Vtx v{q, q, q};
            if (smooth) {
                float lenIn = 0.0f;
                float lenOut = 0.0f;
                if (segOf[i] >= 0) {
                    lenIn = lenOut = handleLen[static_cast<size_t>(segOf[i])];
                } else {
                    const auto vi = static_cast<size_t>(vertexOf[i]);
                    if (c.closed || vi > 0) {
                        lenIn = handleLen[(vi + sc - 1) % sc];
                    }
                    if (c.closed || vi + 1 < n) {
                        lenOut = handleLen[vi % sc];
                    }
                }
                v.in = q - tan[i] * lenIn;
                v.out = q + tan[i] * lenOut;
            }
            r.v.push_back(v);
        }
        writeContour(r, outVerbs, outPoints);
    }
}

void twist(std::span<const PathVerb> verbs, std::span<const float> points, float angleDeg, Vec2 center,
           std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    if (angleDeg == 0.0f || !std::isfinite(angleDeg)) {
        copyPath(verbs, points, outVerbs, outPoints);
        return;
    }
    // T-258a: |angle| at most kMaxTwistDegrees, at most kMaxGeneratedPoints pieces per segment.
    angleDeg = std::clamp(angleDeg, -kMaxTwistDegrees, kMaxTwistDegrees);
    const int pieces = std::clamp(static_cast<int>(std::ceil(std::fabs(angleDeg) / 15.0f)), 1,
                                  static_cast<int>(kMaxGeneratedPoints));
    std::vector<Contour> contours;
    readContours(verbs, points, contours);
    // Subdivide (every segment as a cubic, lines with 1/3 handles).
    std::vector<Contour> split;
    for (const Contour& c : contours) {
        Contour r;
        r.closed = c.closed;
        const size_t sc = segCount(c);
        if (sc == 0) {
            split.push_back(c);
            continue;
        }
        for (size_t j = 0; j < sc; ++j) {
            Seg s = segAt(c, j);
            if (s.line) {
                s.c1 = lerp(s.p0, s.p3, 1.0f / 3.0f);
                s.c2 = lerp(s.p0, s.p3, 2.0f / 3.0f);
                s.line = false;
            }
            for (int k = 0; k < pieces; ++k) {
                const Seg piece = subSegParam(s, static_cast<float>(k) / static_cast<float>(pieces),
                                              static_cast<float>(k + 1) / static_cast<float>(pieces));
                if (r.v.empty()) {
                    r.v.push_back(Vtx{piece.p0, piece.p0, piece.p0});
                }
                r.v.back().out = piece.c1;
                r.v.push_back(Vtx{piece.c2, piece.p3, piece.p3});
            }
        }
        if (c.closed && r.v.size() >= 2) {
            r.v.front().in = r.v.back().in;  // The last piece ends on the first vertex.
            r.v.pop_back();
        } else if (c.closed) {
            r.closed = true;
        }
        split.push_back(std::move(r));
    }
    float maxR = 0.0f;
    for (const Contour& c : split) {
        for (const Vtx& v : c.v) {
            maxR = std::max(maxR, lengthOf(v.p - center));
        }
    }
    for (Contour& c : split) {
        if (maxR > 0.0f) {
            for (Vtx& v : c.v) {
                const float theta = angleDeg * (1.0f - lengthOf(v.p - center) / maxR);
                const Vec2 p = center + rotateDeg(v.p - center, theta);
                v.in = p + rotateDeg(v.in - v.p, theta);
                v.out = p + rotateDeg(v.out - v.p, theta);
                v.p = p;
            }
        }
        writeContour(c, outVerbs, outPoints);
    }
}

void wiggle(std::span<const PathVerb> verbs, std::span<const float> points, const WiggleParams& params,
            float time, uint32_t& ordinal, std::vector<PathVerb>& outVerbs, std::vector<float>& outPoints) {
    if (!std::isfinite(params.detail)) {
        copyPath(verbs, points, outVerbs, outPoints);  // T-258a
        return;
    }
    std::vector<Contour> contours;
    readContours(verbs, points, contours);
    const int pieces = generatedCount(params.detail) + 1;
    constexpr float kSecondAxis = 57.3f;  // noise2 x offset of the y displacement (§31.6.1 K).
    const float y = time * params.speed + params.temporalPhase / 360.0f;
    std::vector<Vec2> pos;
    std::vector<float> arc;
    for (Contour& c : contours) {
        const size_t sc = segCount(c);
        const float xBase = params.spatialPhase / 360.0f + 7.0f * static_cast<float>(ordinal);
        ++ordinal;
        if (sc == 0) {
            writeContour(c, outVerbs, outPoints);
            continue;
        }
        pos.clear();
        arc.clear();
        float cum = 0.0f;
        for (size_t j = 0; j < sc; ++j) {
            const Seg s = segAt(c, j);
            const float len = segLength(s);
            for (int k = 0; k < pieces; ++k) {
                const float f = static_cast<float>(k) / static_cast<float>(pieces);
                pos.push_back(k == 0 ? s.p0 : segPointAtFrac(s, f));
                arc.push_back(cum + len * f);
            }
            cum += len;
        }
        if (!c.closed) {
            pos.push_back(c.v.back().p);
            arc.push_back(cum);
        }
        const size_t count = pos.size();
        for (size_t i = 0; i < count; ++i) {
            const float u = cum > 0.0f ? arc[i] / cum : 0.0f;
            const float x = u * (1.0f - params.correlation) * 4.0f + xBase;
            const Vec2 d{valueNoise2(x, y, params.seed), valueNoise2(x + kSecondAxis, y, params.seed)};
            pos[i] = pos[i] + d * params.size;
        }
        Contour r;
        r.closed = c.closed;
        for (size_t i = 0; i < count; ++i) {
            const Vec2 q = pos[i];
            Vtx v{q, q, q};
            if (params.smooth) {
                const bool hasPrev = c.closed || i > 0;
                const bool hasNext = c.closed || i + 1 < count;
                const Vec2 prev = hasPrev ? pos[(i + count - 1) % count] : q;
                const Vec2 next = hasNext ? pos[(i + 1) % count] : q;
                const Vec2 t = normalized(next - prev);
                v.in = q - t * (lengthOf(q - prev) / 3.0f);
                v.out = q + t * (lengthOf(next - q) / 3.0f);
            }
            r.v.push_back(v);
        }
        writeContour(r, outVerbs, outPoints);
    }
}

void buildShape(ShapeKind kind, const float* p, std::vector<PathVerb>& verbs, std::vector<float>& pts) {
    auto moveTo = [&](Vec2 a) {
        verbs.push_back(PathVerb::Move);
        pushPoint(pts, a);
    };
    auto lineTo = [&](Vec2 a) {
        verbs.push_back(PathVerb::Line);
        pushPoint(pts, a);
    };
    auto cubicTo = [&](Vec2 c1, Vec2 c2, Vec2 a) {
        verbs.push_back(PathVerb::Cubic);
        pushPoint(pts, c1);
        pushPoint(pts, c2);
        pushPoint(pts, a);
    };
    auto close = [&]() { verbs.push_back(PathVerb::Close); };
    switch (kind) {
        case ShapeKind::Rect: {
            // Same geometry as parse_svg.cpp buildShape's <rect>.
            const float x = p[0], y = p[1], w = p[2], h = p[3];
            if (w <= 0.0f || h <= 0.0f) {
                return;
            }
            const float rx = std::clamp(p[4], 0.0f, w * 0.5f);
            const float ry = std::clamp(p[5], 0.0f, h * 0.5f);
            if (rx <= 0.0f || ry <= 0.0f) {
                moveTo(Vec2{x, y});
                lineTo(Vec2{x + w, y});
                lineTo(Vec2{x + w, y + h});
                lineTo(Vec2{x, y + h});
                close();
                return;
            }
            const float ox = rx * kArcKappa;
            const float oy = ry * kArcKappa;
            moveTo(Vec2{x + rx, y});
            lineTo(Vec2{x + w - rx, y});
            cubicTo(Vec2{x + w - rx + ox, y}, Vec2{x + w, y + ry - oy}, Vec2{x + w, y + ry});
            lineTo(Vec2{x + w, y + h - ry});
            cubicTo(Vec2{x + w, y + h - ry + oy}, Vec2{x + w - rx + ox, y + h}, Vec2{x + w - rx, y + h});
            lineTo(Vec2{x + rx, y + h});
            cubicTo(Vec2{x + rx - ox, y + h}, Vec2{x, y + h - ry + oy}, Vec2{x, y + h - ry});
            lineTo(Vec2{x, y + ry});
            cubicTo(Vec2{x, y + ry - oy}, Vec2{x + rx - ox, y}, Vec2{x + rx, y});
            close();
            return;
        }
        case ShapeKind::Ellipse: {
            // Same geometry as parse_svg.cpp buildShape's <ellipse>/<circle>.
            const float cx = p[0], cy = p[1], rx = p[2], ry = p[3];
            if (rx <= 0.0f || ry <= 0.0f) {
                return;
            }
            const float ox = rx * kArcKappa;
            const float oy = ry * kArcKappa;
            moveTo(Vec2{cx + rx, cy});
            cubicTo(Vec2{cx + rx, cy + oy}, Vec2{cx + ox, cy + ry}, Vec2{cx, cy + ry});
            cubicTo(Vec2{cx - ox, cy + ry}, Vec2{cx - rx, cy + oy}, Vec2{cx - rx, cy});
            cubicTo(Vec2{cx - rx, cy - oy}, Vec2{cx - ox, cy - ry}, Vec2{cx, cy - ry});
            cubicTo(Vec2{cx + ox, cy - ry}, Vec2{cx + rx, cy - oy}, Vec2{cx + rx, cy});
            close();
            return;
        }
        case ShapeKind::Star: {
            // Same geometry as parse_svg.cpp buildStar.
            const int n = starPoints(p[2]);  // T-258a: at most kMaxGeneratedPoints.
            const float cx = p[0], cy = p[1], r = p[3], r2 = p[4], rotation = p[5];
            if (n < 3 || r <= 0.0f) {
                return;
            }
            const float round = std::clamp(p[6], 0.0f, 1.0f);
            const float round2 = std::clamp(p[7], 0.0f, 1.0f);
            const bool star = r2 > 0.0f;
            const int count = star ? n * 2 : n;
            const float step = 2.0f * kPi / static_cast<float>(count);
            const float start = (rotation - 90.0f) * kPi / 180.0f;
            std::vector<Vec2> corner(static_cast<size_t>(count));
            std::vector<float> rounds(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                const bool inner = star && (i % 2) == 1;
                const float radius = inner ? r2 : r;
                const float a = start + step * static_cast<float>(i);
                corner[static_cast<size_t>(i)] = Vec2{cx + radius * std::cos(a), cy + radius * std::sin(a)};
                rounds[static_cast<size_t>(i)] = inner ? round2 : round;
            }
            for (int i = 0; i < count; ++i) {
                const Vec2 c = corner[static_cast<size_t>(i)];
                const Vec2 prev = corner[static_cast<size_t>((i + count - 1) % count)];
                const Vec2 next = corner[static_cast<size_t>((i + 1) % count)];
                const float rd = rounds[static_cast<size_t>(i)];
                const float lp = std::sqrt((prev.x - c.x) * (prev.x - c.x) + (prev.y - c.y) * (prev.y - c.y));
                const float ln = std::sqrt((next.x - c.x) * (next.x - c.x) + (next.y - c.y) * (next.y - c.y));
                const float d = rd * std::min(lp, ln) / 2.0f;
                if (rd <= 0.0f || d <= 0.0f || lp <= 0.0f || ln <= 0.0f) {
                    if (i == 0) {
                        moveTo(c);
                    } else {
                        lineTo(c);
                    }
                    continue;
                }
                const Vec2 up{(prev.x - c.x) / lp, (prev.y - c.y) / lp};
                const Vec2 un{(next.x - c.x) / ln, (next.y - c.y) / ln};
                const Vec2 a{c.x + up.x * d, c.y + up.y * d};
                const Vec2 b{c.x + un.x * d, c.y + un.y * d};
                const float h = kKappa * d;
                if (i == 0) {
                    moveTo(a);
                } else {
                    lineTo(a);
                }
                cubicTo(Vec2{a.x - up.x * h, a.y - up.y * h}, Vec2{b.x - un.x * h, b.y - un.y * h}, b);
            }
            close();
            return;
        }
        default:
            return;
    }
}

bool shapeBounds(ShapeKind kind, const float* p, Rect& out) {
    switch (kind) {
        case ShapeKind::Rect:
            if (p[2] <= 0.0f || p[3] <= 0.0f) {
                return false;
            }
            out = Rect{p[0], p[1], p[2], p[3]};
            return true;
        case ShapeKind::Ellipse:
            if (p[2] <= 0.0f || p[3] <= 0.0f) {
                return false;
            }
            out = Rect{p[0] - p[2], p[1] - p[3], 2.0f * p[2], 2.0f * p[3]};
            return true;
        case ShapeKind::Star: {
            if (starPoints(p[2]) < 3 || p[3] <= 0.0f) {
                return false;
            }
            const float r = std::max(p[3], p[4]);
            out = Rect{p[0] - r, p[1] - r, 2.0f * r, 2.0f * r};
            return true;
        }
        default:
            return false;
    }
}

uint32_t vertexCount(std::span<const PathVerb> verbs) {
    uint32_t n = 0;
    for (PathVerb v : verbs) {
        if (v != PathVerb::Close) {
            ++n;
        }
    }
    return n;
}

}  // namespace xt::modifiers
