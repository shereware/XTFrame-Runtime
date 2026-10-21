#include <cmath>

#include "xt/geom.h"

/// Dash pattern application on flattened polylines.
/// See docs/design/10-backends.md §10.1

namespace xt {

namespace {

/// Most pattern periods one contour is dashed with (T-258a); a contour needing more (a tiny or
/// denormal pattern) is stroked solid, as if there were no dash.
constexpr float kMaxDashCycles = 100000.0f;

// Appends the polyline points covering the closed arc-length range [s0, s1] (s0 <= s1) of the
// contour described by `ext` (points, including the synthetic closing point for closed input
// contours) and `cum` (cumulative arc length per point, cum.size() == ext.size()).
// A zero-length range (s0 == s1) still appends a single point.
void appendRange(std::span<const Vec2> ext, const std::vector<float>& cum, float s0, float s1,
                 std::vector<Vec2>& out) {
    auto pointAt = [&](float s) -> Vec2 {
        if (s <= cum.front()) {
            return ext.front();
        }
        if (s >= cum.back()) {
            return ext.back();
        }
        size_t lo = 0;
        size_t hi = cum.size() - 1;
        while (lo + 1 < hi) {
            size_t mid = (lo + hi) / 2;
            if (cum[mid] <= s) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        float segLen = cum[hi] - cum[lo];
        float t = segLen > 1e-12f ? (s - cum[lo]) / segLen : 0.0f;
        return ext[lo] + (ext[hi] - ext[lo]) * t;
    };

    out.push_back(pointAt(s0));
    if (s1 > s0) {
        for (size_t i = 0; i < cum.size(); ++i) {
            if (cum[i] > s0 && cum[i] < s1) {
                out.push_back(ext[i]);
            }
        }
        out.push_back(pointAt(s1));
    }
}

}  // namespace

void applyDash(std::span<const Vec2> points, std::span<const uint32_t> contourStarts,
               std::span<const uint8_t> closed, std::span<const float> dashes, float phase,
               std::vector<Vec2>& outPoints, std::vector<uint32_t>& outContourStarts,
               std::vector<uint8_t>& outClosed) {
    size_t m = dashes.size();
    std::vector<float> pat;
    pat.reserve(m * 2);
    for (float v : dashes) {
        pat.push_back(v > 0.0f ? v : 0.0f);
    }
    if (m % 2 != 0) {
        for (float v : dashes) {
            pat.push_back(v > 0.0f ? v : 0.0f);
        }
    }
    size_t patCount = pat.size();

    float total = 0.0f;
    for (float v : pat) {
        total += v;
    }

    size_t contourCount = contourStarts.size();
    bool passThrough = pat.empty() || !(total > 0.0f) || !std::isfinite(total);

    for (size_t ci = 0; ci < contourCount; ++ci) {
        uint32_t start = contourStarts[ci];
        uint32_t end = (ci + 1 < contourCount) ? contourStarts[ci + 1] : static_cast<uint32_t>(points.size());
        if (end <= start) {
            continue;
        }
        size_t n = static_cast<size_t>(end - start);
        bool isClosed = closed[ci] != 0;
        std::span<const Vec2> contourPts = points.subspan(start, n);

        auto solid = [&]() {
            outContourStarts.push_back(static_cast<uint32_t>(outPoints.size()));
            for (const Vec2& p : contourPts) {
                outPoints.push_back(p);
            }
            outClosed.push_back(isClosed ? 1 : 0);
        };
        if (passThrough) {
            solid();
            continue;
        }

        // Extended point list: closed contours append the first point again as a virtual end,
        // per spec a closed contour is treated as an open one starting at its first point.
        std::vector<Vec2> ext(contourPts.begin(), contourPts.end());
        if (isClosed) {
            ext.push_back(contourPts.front());
        }
        if (ext.size() < 2) {
            continue;  // nothing to dash on a single-point contour
        }
        std::vector<float> cum(ext.size(), 0.0f);
        for (size_t i = 1; i < ext.size(); ++i) {
            Vec2 d = ext[i] - ext[i - 1];
            cum[i] = cum[i - 1] + std::sqrt(d.x * d.x + d.y * d.y);
        }
        float totalLen = cum.back();
        if (!(totalLen > 0.0f)) {
            continue;
        }
        // T-258a: more than kMaxDashCycles periods (or a non-finite count) strokes the contour solid.
        const float cycles = totalLen / total;
        if (!(cycles <= kMaxDashCycles)) {
            solid();
            continue;
        }

        float phaseMod = std::fmod(phase, total);
        if (phaseMod < 0.0f) {
            phaseMod += total;
        }
        // Locate the pattern entry containing `phaseMod`.
        size_t idx = 0;
        float acc = 0.0f;
        for (size_t i = 0; i < patCount; ++i) {
            if (phaseMod < acc + pat[i] || i + 1 == patCount) {
                idx = i;
                break;
            }
            acc += pat[i];
        }
        float uPos = phase;  // absolute pattern-space position matching arc-length s=0
        float remainingInEntry = (acc + pat[idx]) - phaseMod;
        if (remainingInEntry < 0.0f) {
            remainingInEntry = 0.0f;
        }

        float uEnd = phase + totalLen;
        size_t guard = 0;
        float cyclesF = std::ceil(cycles) + 2.0f;  // Finite and <= kMaxDashCycles + 2 here.
        size_t maxIters = patCount * static_cast<size_t>(cyclesF) + patCount + 8;
        while (guard++ < maxIters) {
            float segLen = remainingInEntry;
            float entryEndU = uPos + segLen;
            float clippedEndU = entryEndU < uEnd ? entryEndU : uEnd;
            float s0 = uPos - phase;
            float s1 = clippedEndU - phase;
            bool isOn = (idx % 2) == 0;
            if (isOn) {
                std::vector<Vec2> seg;
                appendRange(ext, cum, s0, s1, seg);
                outContourStarts.push_back(static_cast<uint32_t>(outPoints.size()));
                for (const Vec2& p : seg) {
                    outPoints.push_back(p);
                }
                outClosed.push_back(0);
            }
            bool reachedEnd = uPos >= uEnd || clippedEndU >= uEnd;
            bool exactBoundary = std::fabs(clippedEndU - entryEndU) < 1e-6f;
            uPos = clippedEndU;
            if (reachedEnd) {
                if (exactBoundary) {
                    size_t nextIdx = (idx + 1) % patCount;
                    if ((nextIdx % 2) == 0) {
                        // Trailing zero-length "on" run exactly at the contour end.
                        outContourStarts.push_back(static_cast<uint32_t>(outPoints.size()));
                        outPoints.push_back(ext.back());
                        outClosed.push_back(0);
                    }
                }
                break;
            }
            idx = (idx + 1) % patCount;
            remainingInEntry = pat[idx];
        }
    }
}

}  // namespace xt
