#include "rounded_rect.h"

#include <algorithm>

namespace xt {

namespace {

void pushVerb(std::vector<PathVerb>& verbs, PathVerb v) { verbs.push_back(v); }

void pushPoint(std::vector<float>& points, Vec2 p) {
    points.push_back(p.x);
    points.push_back(p.y);
}

}  // namespace

void clampRoundedRectRadii(float w, float h, float& tl, float& tr, float& br, float& bl) {
    float half = std::min(w, h) * 0.5f;
    if (half < 0.0f) {
        half = 0.0f;
    }
    tl = std::clamp(tl, 0.0f, half);
    tr = std::clamp(tr, 0.0f, half);
    br = std::clamp(br, 0.0f, half);
    bl = std::clamp(bl, 0.0f, half);

    float scale = 1.0f;
    // Top edge: tl + tr <= w. Right edge: tr + br <= h. Bottom edge: bl + br <= w. Left edge:
    // tl + bl <= h.
    auto edgeScale = [](float sum, float length) {
        return (sum > length && sum > 0.0f) ? (length / sum) : 1.0f;
    };
    scale = std::min(scale, edgeScale(tl + tr, w));
    scale = std::min(scale, edgeScale(tr + br, h));
    scale = std::min(scale, edgeScale(bl + br, w));
    scale = std::min(scale, edgeScale(tl + bl, h));

    if (scale < 1.0f) {
        tl *= scale;
        tr *= scale;
        br *= scale;
        bl *= scale;
    }
}

void appendRoundedRectPath(Rect box, float rTL, float rTR, float rBR, float rBL, std::vector<PathVerb>& verbs,
                           std::vector<float>& points) {
    float x = box.x;
    float y = box.y;
    float w = box.w;
    float h = box.h;
    float k = kRoundedRectKappa;

    Vec2 start{x + rTL, y};             // Top edge start, just after the TL corner.
    Vec2 beforeTR{x + w - rTR, y};      // Top edge end, just before the TR corner.
    Vec2 afterTR{x + w, y + rTR};       // Right edge start, just after the TR corner.
    Vec2 beforeBR{x + w, y + h - rBR};  // Right edge end, just before the BR corner.
    Vec2 afterBR{x + w - rBR, y + h};   // Bottom edge start, just after the BR corner.
    Vec2 beforeBL{x + rBL, y + h};      // Bottom edge end, just before the BL corner.
    Vec2 afterBL{x, y + h - rBL};       // Left edge start, just after the BL corner.
    Vec2 beforeTL{x, y + rTL};          // Left edge end, just before the TL corner.

    pushVerb(verbs, PathVerb::Move);
    pushPoint(points, start);

    // TR corner.
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeTR);
    if (rTR > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeTR.x + rTR * k, beforeTR.y});
        pushPoint(points, Vec2{afterTR.x, afterTR.y - rTR * k});
        pushPoint(points, afterTR);
    }

    // BR corner.
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeBR);
    if (rBR > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeBR.x, beforeBR.y + rBR * k});
        pushPoint(points, Vec2{afterBR.x + rBR * k, afterBR.y});
        pushPoint(points, afterBR);
    }

    // BL corner.
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeBL);
    if (rBL > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeBL.x - rBL * k, beforeBL.y});
        pushPoint(points, Vec2{afterBL.x, afterBL.y + rBL * k});
        pushPoint(points, afterBL);
    }

    // TL corner (closes back onto `start`).
    pushVerb(verbs, PathVerb::Line);
    pushPoint(points, beforeTL);
    if (rTL > 0.0f) {
        pushVerb(verbs, PathVerb::Cubic);
        pushPoint(points, Vec2{beforeTL.x, beforeTL.y - rTL * k});
        pushPoint(points, Vec2{start.x - rTL * k, start.y});
        pushPoint(points, start);
    }

    pushVerb(verbs, PathVerb::Close);
}

/// T-201 (moved from painter_element.cpp by T-680, which also needs it in xt::gpu):
/// appendRoundedRectPath()'s outline traced the other way round (counter-clockwise: TL
/// corner, left edge, BL, bottom, BR, right edge, TR, top edge), so a ring of an outer and this
/// inner outline fills with the nonzero rule and a side of zero width -- where the two outlines
/// coincide -- cancels exactly instead of leaving an antialiasing seam (even-odd adds the two
/// coincident edges' coverage).
void appendRoundedRectPathReversed(Rect box, float rTL, float rTR, float rBR, float rBL,
                                   std::vector<PathVerb>& verbs, std::vector<float>& points) {
    const float x = box.x;
    const float y = box.y;
    const float w = box.w;
    const float h = box.h;
    const float k = kRoundedRectKappa;
    auto point = [&points](float px, float py) {
        points.push_back(px);
        points.push_back(py);
    };
    auto line = [&](float px, float py) {
        verbs.push_back(PathVerb::Line);
        point(px, py);
    };
    auto cubic = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
        verbs.push_back(PathVerb::Cubic);
        point(x1, y1);
        point(x2, y2);
        point(x3, y3);
    };
    verbs.push_back(PathVerb::Move);
    point(x + rTL, y);
    if (rTL > 0.0f) {
        cubic(x + rTL - rTL * k, y, x, y + rTL - rTL * k, x, y + rTL);
    }
    line(x, y + h - rBL);
    if (rBL > 0.0f) {
        cubic(x, y + h - rBL + rBL * k, x + rBL - rBL * k, y + h, x + rBL, y + h);
    }
    line(x + w - rBR, y + h);
    if (rBR > 0.0f) {
        cubic(x + w - rBR + rBR * k, y + h, x + w, y + h - rBR + rBR * k, x + w, y + h - rBR);
    }
    line(x + w, y + rTR);
    if (rTR > 0.0f) {
        cubic(x + w, y + rTR - rTR * k, x + w - rTR + rTR * k, y, x + w - rTR, y);
    }
    line(x + rTL, y);
    verbs.push_back(PathVerb::Close);
}

}  // namespace xt
