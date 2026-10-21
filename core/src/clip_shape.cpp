#include "clip_shape.h"

#include <algorithm>
#include <cmath>

#include "rounded_rect.h"  // kRoundedRectKappa.

namespace xt::clip_shape {

namespace {

/// A px / % length against `ref` (its CSS reference length); any other unit reads as px (the
/// parsers only produce px and %, the IR validator rejects the rest).
float resolveLen(const Length& len, float ref) {
    if (len.unit == LengthUnit::Percent) {
        return len.value * 0.01f * ref;
    }
    return len.value;
}

/// A circle / ellipse radius: `closest-side` / `farthest-side` from the center's distances to the
/// two edges along the axis (`lo` = distance to the near edge, `hi` = to the far edge), else a
/// length against `ref`. Never negative.
float resolveRadius(const Length& len, float ref, float lo, float hi) {
    float r;
    if (len.unit == LengthUnit::Auto) {
        r = len.value == kClipRadiusFarthestSide ? std::max(lo, hi) : std::min(lo, hi);
    } else {
        r = resolveLen(len, ref);
    }
    return std::isfinite(r) ? std::max(0.0f, r) : 0.0f;
}

void pushPoint(std::vector<float>& points, Vec2 p) {
    points.push_back(p.x);
    points.push_back(p.y);
}

/// One quarter-ellipse arc from `from` to `to` around `center` with half-axes rx / ry, as a cubic
/// (the standard 4-segment kappa approximation, as rounded_rect.cpp draws its corners).
void pushQuarterArc(std::vector<PathVerb>& verbs, std::vector<float>& points, Vec2 from, Vec2 to,
                    Vec2 center) {
    const float k = kRoundedRectKappa;
    // The arc's tangents: at `from` the tangent points towards the corner (the point sharing
    // from's offset axis with to), at `to` it points from that corner towards `to`.
    const Vec2 corner{from.x == center.x ? to.x : from.x, from.y == center.y ? to.y : from.y};
    const Vec2 p1{from.x + (corner.x - from.x) * k, from.y + (corner.y - from.y) * k};
    const Vec2 p2{to.x + (corner.x - to.x) * k, to.y + (corner.y - to.y) * k};
    verbs.push_back(PathVerb::Cubic);
    pushPoint(points, p1);
    pushPoint(points, p2);
    pushPoint(points, to);
}

/// Whether `p` is inside the axis-aligned ellipse (center c, half-axes rx / ry); a zero half-axis
/// contains nothing.
bool insideEllipse(Vec2 c, float rx, float ry, Vec2 p) {
    if (!(rx > 0.0f) || !(ry > 0.0f)) {
        return false;
    }
    const float dx = (p.x - c.x) / rx;
    const float dy = (p.y - c.y) / ry;
    return dx * dx + dy * dy <= 1.0f;
}

}  // namespace

bool resolveInset(const ElementClipShape& shape, Rect box, ResolvedInset& out) {
    if (shape.kind != ClipShapeKind::Inset || shape.params.size() != kClipInsetParamCount) {
        return false;
    }
    const float top = resolveLen(shape.params[0], box.h);
    const float right = resolveLen(shape.params[1], box.w);
    const float bottom = resolveLen(shape.params[2], box.h);
    const float left = resolveLen(shape.params[3], box.w);
    out.rect = Rect{box.x + left, box.y + top, box.w - left - right, box.h - top - bottom};
    if (!std::isfinite(out.rect.x) || !std::isfinite(out.rect.y) || !std::isfinite(out.rect.w) ||
        !std::isfinite(out.rect.h)) {
        out.rect = Rect{box.x, box.y, 0, 0};
    }
    for (int i = 0; i < 8; ++i) {
        // Even slots are horizontal radii (% of the inset rect's width), odd ones vertical.
        const float ref = (i % 2 == 0) ? out.rect.w : out.rect.h;
        const float r = resolveLen(shape.params[4 + static_cast<size_t>(i)], ref);
        out.radii[i] = std::isfinite(r) ? std::max(0.0f, r) : 0.0f;
    }
    if (out.rect.w <= 0.0f || out.rect.h <= 0.0f) {
        return true;  // Empty; the caller treats it as "clip everything".
    }
    // CSS Backgrounds 3 §5.5: scale every radius by the smallest factor f <= 1 that keeps each
    // edge's adjacent radii within its length.
    float f = 1.0f;
    const float sums[4] = {out.radii[0] + out.radii[2], out.radii[5] + out.radii[7],
                           out.radii[4] + out.radii[6], out.radii[1] + out.radii[3]};
    const float lengths[4] = {out.rect.w, out.rect.h, out.rect.w, out.rect.h};
    for (int i = 0; i < 4; ++i) {
        if (sums[i] > lengths[i] && sums[i] > 0.0f) {
            f = std::min(f, lengths[i] / sums[i]);
        }
    }
    if (f < 1.0f) {
        for (float& r : out.radii) {
            r *= f;
        }
    }
    return true;
}

bool resolveEllipse(const ElementClipShape& shape, Rect box, ResolvedEllipse& out) {
    size_t centerIndex;
    if (shape.kind == ClipShapeKind::Circle && shape.params.size() == kClipCircleParamCount) {
        centerIndex = 1;
    } else if (shape.kind == ClipShapeKind::Ellipse && shape.params.size() == kClipEllipseParamCount) {
        centerIndex = 2;
    } else {
        return false;
    }
    const float cx = resolveLen(shape.params[centerIndex], box.w);
    const float cy = resolveLen(shape.params[centerIndex + 1], box.h);
    out.center = Vec2{box.x + cx, box.y + cy};
    const float dl = std::fabs(cx);
    const float dr = std::fabs(box.w - cx);
    const float dt = std::fabs(cy);
    const float db = std::fabs(box.h - cy);
    if (shape.kind == ClipShapeKind::Circle) {
        // CSS Shapes 1: a `%` radius is of sqrt(w^2 + h^2) / sqrt(2); the side keywords look at
        // all four edges.
        const float ref = std::sqrt((box.w * box.w + box.h * box.h) * 0.5f);
        const float lo = std::min(std::min(dl, dr), std::min(dt, db));
        const float hi = std::max(std::max(dl, dr), std::max(dt, db));
        out.rx = out.ry = resolveRadius(shape.params[0], ref, lo, hi);
    } else {
        out.rx = resolveRadius(shape.params[0], box.w, dl, dr);
        out.ry = resolveRadius(shape.params[1], box.h, dt, db);
    }
    if (!std::isfinite(out.center.x) || !std::isfinite(out.center.y)) {
        out.center = Vec2{box.x, box.y};
        out.rx = out.ry = 0.0f;
    }
    return true;
}

bool resolvePolygon(const ElementClipShape& shape, Rect box, std::vector<Vec2>& out) {
    out.clear();
    if (shape.kind != ClipShapeKind::Polygon || shape.params.size() < 6 || (shape.params.size() % 2) != 0) {
        return false;
    }
    const size_t n = std::min(shape.params.size() / 2, static_cast<size_t>(kMaxClipPolygonVertices));
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Vec2 p{box.x + resolveLen(shape.params[2 * i], box.w),
               box.y + resolveLen(shape.params[2 * i + 1], box.h)};
        if (!std::isfinite(p.x) || !std::isfinite(p.y)) {
            p = Vec2{box.x, box.y};
        }
        out.push_back(p);
    }
    return true;
}

bool appendPath(const ElementClipShape& shape, Rect box, std::vector<PathVerb>& verbs,
                std::vector<float>& points, FillRule& rule) {
    rule = FillRule::NonZero;
    switch (shape.kind) {
        case ClipShapeKind::Inset: {
            ResolvedInset in;
            if (!resolveInset(shape, box, in) || in.rect.w <= 0.0f || in.rect.h <= 0.0f) {
                return false;
            }
            const Rect r = in.rect;
            const float* rad = in.radii;
            const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h;
            // Clockwise from the top edge, each corner a Line to the arc start then a Cubic when
            // both of its radii are > 0 (as appendRoundedRectPath does for circular corners).
            verbs.push_back(PathVerb::Move);
            pushPoint(points, Vec2{x0 + rad[0], y0});
            verbs.push_back(PathVerb::Line);
            pushPoint(points, Vec2{x1 - rad[2], y0});
            if (rad[2] > 0.0f && rad[3] > 0.0f) {
                pushQuarterArc(verbs, points, Vec2{x1 - rad[2], y0}, Vec2{x1, y0 + rad[3]},
                               Vec2{x1 - rad[2], y0 + rad[3]});
            }
            verbs.push_back(PathVerb::Line);
            pushPoint(points, Vec2{x1, y1 - rad[5]});
            if (rad[4] > 0.0f && rad[5] > 0.0f) {
                pushQuarterArc(verbs, points, Vec2{x1, y1 - rad[5]}, Vec2{x1 - rad[4], y1},
                               Vec2{x1 - rad[4], y1 - rad[5]});
            }
            verbs.push_back(PathVerb::Line);
            pushPoint(points, Vec2{x0 + rad[6], y1});
            if (rad[6] > 0.0f && rad[7] > 0.0f) {
                pushQuarterArc(verbs, points, Vec2{x0 + rad[6], y1}, Vec2{x0, y1 - rad[7]},
                               Vec2{x0 + rad[6], y1 - rad[7]});
            }
            verbs.push_back(PathVerb::Line);
            pushPoint(points, Vec2{x0, y0 + rad[1]});
            if (rad[0] > 0.0f && rad[1] > 0.0f) {
                pushQuarterArc(verbs, points, Vec2{x0, y0 + rad[1]}, Vec2{x0 + rad[0], y0},
                               Vec2{x0 + rad[0], y0 + rad[1]});
            }
            verbs.push_back(PathVerb::Close);
            return true;
        }
        case ClipShapeKind::Circle:
        case ClipShapeKind::Ellipse: {
            ResolvedEllipse e;
            if (!resolveEllipse(shape, box, e) || !(e.rx > 0.0f) || !(e.ry > 0.0f)) {
                return false;
            }
            const Vec2 c = e.center;
            const Vec2 top{c.x, c.y - e.ry};
            const Vec2 right{c.x + e.rx, c.y};
            const Vec2 bottom{c.x, c.y + e.ry};
            const Vec2 left{c.x - e.rx, c.y};
            verbs.push_back(PathVerb::Move);
            pushPoint(points, top);
            pushQuarterArc(verbs, points, top, right, c);
            pushQuarterArc(verbs, points, right, bottom, c);
            pushQuarterArc(verbs, points, bottom, left, c);
            pushQuarterArc(verbs, points, left, top, c);
            verbs.push_back(PathVerb::Close);
            return true;
        }
        case ClipShapeKind::Polygon: {
            std::vector<Vec2> pts;
            if (!resolvePolygon(shape, box, pts) || pts.size() < 3) {
                return false;
            }
            rule = FillRule::EvenOdd;
            verbs.push_back(PathVerb::Move);
            pushPoint(points, pts[0]);
            for (size_t i = 1; i < pts.size(); ++i) {
                verbs.push_back(PathVerb::Line);
                pushPoint(points, pts[i]);
            }
            verbs.push_back(PathVerb::Close);
            return true;
        }
        case ClipShapeKind::None:
        default:
            return false;
    }
}

Rect bounds(const ElementClipShape& shape, Rect box) {
    switch (shape.kind) {
        case ClipShapeKind::Inset: {
            ResolvedInset in;
            if (!resolveInset(shape, box, in) || in.rect.w <= 0.0f || in.rect.h <= 0.0f) {
                return Rect{box.x, box.y, 0, 0};
            }
            return in.rect;
        }
        case ClipShapeKind::Circle:
        case ClipShapeKind::Ellipse: {
            ResolvedEllipse e;
            if (!resolveEllipse(shape, box, e) || !(e.rx > 0.0f) || !(e.ry > 0.0f)) {
                return Rect{box.x, box.y, 0, 0};
            }
            return Rect{e.center.x - e.rx, e.center.y - e.ry, 2.0f * e.rx, 2.0f * e.ry};
        }
        case ClipShapeKind::Polygon: {
            std::vector<Vec2> pts;
            if (!resolvePolygon(shape, box, pts) || pts.size() < 3) {
                return Rect{box.x, box.y, 0, 0};
            }
            float x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
            for (const Vec2& p : pts) {
                x0 = std::min(x0, p.x);
                y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x);
                y1 = std::max(y1, p.y);
            }
            return Rect{x0, y0, x1 - x0, y1 - y0};
        }
        case ClipShapeKind::None:
        default:
            return Rect{box.x, box.y, 0, 0};
    }
}

bool contains(const ElementClipShape& shape, Rect box, Vec2 p) {
    switch (shape.kind) {
        case ClipShapeKind::None:
            return true;
        case ClipShapeKind::Inset: {
            ResolvedInset in;
            if (!resolveInset(shape, box, in) || in.rect.w <= 0.0f || in.rect.h <= 0.0f) {
                return false;
            }
            const Rect r = in.rect;
            if (p.x < r.x || p.x > r.x + r.w || p.y < r.y || p.y > r.y + r.h) {
                return false;
            }
            // Inside the rectangle: reject the point when it falls in a rounded corner's outside.
            const float* rad = in.radii;
            const struct {
                float cx, cy, rx, ry;
                bool left, top;
            } corners[4] = {
                {r.x + rad[0], r.y + rad[1], rad[0], rad[1], true, true},
                {r.x + r.w - rad[2], r.y + rad[3], rad[2], rad[3], false, true},
                {r.x + r.w - rad[4], r.y + r.h - rad[5], rad[4], rad[5], false, false},
                {r.x + rad[6], r.y + r.h - rad[7], rad[6], rad[7], true, false},
            };
            for (const auto& c : corners) {
                if (!(c.rx > 0.0f) || !(c.ry > 0.0f)) {
                    continue;
                }
                const bool inX = c.left ? p.x < c.cx : p.x > c.cx;
                const bool inY = c.top ? p.y < c.cy : p.y > c.cy;
                if (inX && inY && !insideEllipse(Vec2{c.cx, c.cy}, c.rx, c.ry, p)) {
                    return false;
                }
            }
            return true;
        }
        case ClipShapeKind::Circle:
        case ClipShapeKind::Ellipse: {
            ResolvedEllipse e;
            return resolveEllipse(shape, box, e) && insideEllipse(e.center, e.rx, e.ry, p);
        }
        case ClipShapeKind::Polygon: {
            std::vector<Vec2> pts;
            if (!resolvePolygon(shape, box, pts) || pts.size() < 3) {
                return false;
            }
            // Even-odd crossing count along the +x ray.
            bool inside = false;
            for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
                const Vec2& a = pts[i];
                const Vec2& b = pts[j];
                if ((a.y > p.y) != (b.y > p.y)) {
                    const float x = a.x + (p.y - a.y) * (b.x - a.x) / (b.y - a.y);
                    if (p.x < x) {
                        inside = !inside;
                    }
                }
            }
            return inside;
        }
        default:
            return false;
    }
}

}  // namespace xt::clip_shape
