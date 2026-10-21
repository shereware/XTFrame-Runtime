#include "delaunay.h"

#include <cmath>
#include <cstddef>

/// See delaunay.h and docs/design/32-rigging-and-deformation.md §32.2.4 (T-243).

namespace xt {

namespace {

struct DPoint {
    double x = 0;
    double y = 0;
};

struct DTri {
    uint32_t v[3] = {0, 0, 0};  ///< Counter-clockwise (positive orientation).
};

struct DEdge {
    uint32_t a = 0;
    uint32_t b = 0;
};

/// Twice the signed area of (a, b, c); positive = counter-clockwise.
double orient(const DPoint& a, const DPoint& b, const DPoint& c) {
    return (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
}

/// > 0 when `d` is strictly inside the circumcircle of the counter-clockwise triangle (a, b, c).
double inCircle(const DPoint& a, const DPoint& b, const DPoint& c, const DPoint& d) {
    const double adx = a.x - d.x, ady = a.y - d.y;
    const double bdx = b.x - d.x, bdy = b.y - d.y;
    const double cdx = c.x - d.x, cdy = c.y - d.y;
    const double ad = adx * adx + ady * ady;
    const double bd = bdx * bdx + bdy * bdy;
    const double cd = cdx * cdx + cdy * cdy;
    return adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) + ad * (bdx * cdy - bdy * cdx);
}

}  // namespace

bool triangulate(std::span<const Vec2> points, std::vector<uint16_t>& outIndices) {
    outIndices.clear();
    if (points.size() < 3 || points.size() > 65536) {
        return false;
    }

    // Normalize into the unit box with one uniform scale (circles stay circles).
    double minX = points[0].x, minY = points[0].y, maxX = minX, maxY = minY;
    for (const Vec2& p : points) {
        minX = std::fmin(minX, p.x);
        minY = std::fmin(minY, p.y);
        maxX = std::fmax(maxX, p.x);
        maxY = std::fmax(maxY, p.y);
    }
    const double extent = std::fmax(maxX - minX, maxY - minY);
    if (!(extent > 0.0)) {
        return false;  // Every point coincides.
    }

    // Merge duplicates: `uniq` holds the first point of every coincident set.
    constexpr double kSameEps2 = 1e-18;
    std::vector<DPoint> pts;
    std::vector<uint32_t> source;  // pts index -> points index
    pts.reserve(points.size() + 3);
    for (size_t i = 0; i < points.size(); ++i) {
        const DPoint p{(points[i].x - minX) / extent, (points[i].y - minY) / extent};
        bool dup = false;
        for (const DPoint& q : pts) {
            const double dx = p.x - q.x, dy = p.y - q.y;
            if (dx * dx + dy * dy <= kSameEps2) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            pts.push_back(p);
            source.push_back(static_cast<uint32_t>(i));
        }
    }
    const size_t n = pts.size();
    if (n < 3) {
        return false;
    }

    // All collinear: no point is off the line through the two farthest-apart candidates.
    size_t far = 1;
    double farD = 0;
    for (size_t i = 1; i < n; ++i) {
        const double dx = pts[i].x - pts[0].x, dy = pts[i].y - pts[0].y;
        if (dx * dx + dy * dy > farD) {
            farD = dx * dx + dy * dy;
            far = i;
        }
    }
    constexpr double kCollinearEps = 1e-12;
    bool collinear = true;
    for (size_t i = 1; i < n && collinear; ++i) {
        collinear = std::fabs(orient(pts[0], pts[far], pts[i])) <= kCollinearEps;
    }
    if (collinear) {
        return false;
    }

    // Super triangle (counter-clockwise), far outside the unit box.
    constexpr double kSuper = 1e5;
    const uint32_t s0 = static_cast<uint32_t>(n);
    pts.push_back(DPoint{-kSuper, -kSuper});
    pts.push_back(DPoint{kSuper, -kSuper});
    pts.push_back(DPoint{0.5, kSuper});
    std::vector<DTri> tris;
    tris.push_back(DTri{{s0, s0 + 1, s0 + 2}});

    std::vector<DTri> kept;
    std::vector<DEdge> cavity;
    for (uint32_t pi = 0; pi < n; ++pi) {
        const DPoint& p = pts[pi];
        kept.clear();
        cavity.clear();
        for (const DTri& t : tris) {
            if (inCircle(pts[t.v[0]], pts[t.v[1]], pts[t.v[2]], p) > 0.0) {
                // A bad triangle: its edges bound the cavity unless another bad triangle shares
                // them (then they appear reversed and cancel).
                for (int k = 0; k < 3; ++k) {
                    const DEdge e{t.v[k], t.v[(k + 1) % 3]};
                    bool shared = false;
                    for (size_t m = 0; m < cavity.size(); ++m) {
                        if (cavity[m].a == e.b && cavity[m].b == e.a) {
                            cavity[m] = cavity.back();
                            cavity.pop_back();
                            shared = true;
                            break;
                        }
                    }
                    if (!shared) {
                        cavity.push_back(e);
                    }
                }
            } else {
                kept.push_back(t);
            }
        }
        if (cavity.empty()) {
            continue;  // Numerically on no circumcircle (cannot happen for an interior point).
        }
        for (const DEdge& e : cavity) {
            kept.push_back(DTri{{e.a, e.b, pi}});
        }
        tris.swap(kept);
    }

    for (const DTri& t : tris) {
        if (t.v[0] >= s0 || t.v[1] >= s0 || t.v[2] >= s0) {
            continue;
        }
        if (!(orient(pts[t.v[0]], pts[t.v[1]], pts[t.v[2]]) > 0.0)) {
            continue;  // A degenerate sliver.
        }
        for (int k = 0; k < 3; ++k) {
            outIndices.push_back(static_cast<uint16_t>(source[t.v[k]]));
        }
    }
    if (outIndices.empty()) {
        return false;
    }
    return true;
}

}  // namespace xt
