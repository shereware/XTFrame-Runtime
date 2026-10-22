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

}  // namespace xt
