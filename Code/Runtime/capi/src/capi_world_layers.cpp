#include <algorithm>
#include <cmath>
#include <string>

#include "capi_internal.h"

/// `xt_world_layer_orbit_matrix` and `xt_world_layer_unproject` (Docs/design/50-c-abi.md §50.af items 5-6,
/// Docs/tasks/T-709.md item 2): the camera helpers of the world-layer functions, pure math, compiled in
/// every build (with and without wgpu). The orbit camera is the one of `xtframe-view --layers-view`
/// (Code/Tools/view/view_layers.cpp LayerOrbit::frame / boxToSurface / ray), copied here -- the C ABI
/// library does not link the tool -- with its depth scale left out (draw_layers_gpu and unproject take it
/// separately) and the field of view added: `fov_deg` is the angle the box's longer side subtends, and
/// fov_deg <= 0 is LayerOrbit's own focal length (kFill x the distance).

using xt::capi::fail;

namespace {

constexpr float kDegToRad = 3.14159265358979f / 180.0f;
/// LayerOrbit's camera distance in multiples of the root box's longer side, and how much of the frame
/// the unturned box takes (view_layers.cpp kDistance / kFill).
constexpr float kDistance = 1.8f;
constexpr float kFill = 0.74f;

struct V3 {
    float x = 0, y = 0, z = 0;
};

V3 cross(V3 a, V3 b) { return V3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 normalize(V3 v) {
    const float len = std::sqrt(dot(v, v));
    return len > 0.0f ? V3{v.x / len, v.y / len, v.z / len} : v;
}

bool finite(float v) { return std::isfinite(v); }

/// How far in front of the root plane an orthographic ray starts (root-box px, before the depth scale):
/// beyond any layer a UI puts in front of its root box.
constexpr float kOrthoOriginZ = 1.0e5f;

}  // namespace

extern "C" {

XT_CAPI xt_status xt_world_layer_orbit_matrix(float out[16], float box_w, float box_h, float dest_w,
                                              float dest_h, float yaw_deg, float pitch_deg, float fov_deg) {
    XT_CAPI_GUARD("xt_world_layer_orbit_matrix", {
        if (out == nullptr)
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_world_layer_orbit_matrix: out is NULL");
        if (!finite(box_w) || !finite(box_h) || !finite(dest_w) || !finite(dest_h) || !(box_w > 0.0f) ||
            !(box_h > 0.0f) || !(dest_w > 0.0f) || !(dest_h > 0.0f) || !finite(yaw_deg) ||
            !finite(pitch_deg)) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_world_layer_orbit_matrix: sizes must be > 0 and finite, angles finite");
        }
        // LayerOrbit::frame: `back` points from the target to the eye, `right` and `down` span the image.
        const float yaw = yaw_deg * kDegToRad;
        const float pitch = pitch_deg * kDegToRad;
        const V3 back{std::sin(yaw) * std::cos(pitch), -std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
        const V3 right = normalize(cross(V3{0, 1, 0}, back));
        const V3 down = cross(back, right);
        const float longer = std::max(box_w, box_h);
        const float distance = kDistance * longer;
        const V3 eye{box_w * 0.5f + back.x * distance, box_h * 0.5f + back.y * distance, back.z * distance};
        float focal = kFill * distance;
        if (finite(fov_deg) && fov_deg > 0.0f) {
            const float half = std::tan(std::min(fov_deg, 179.0f) * 0.5f * kDegToRad);
            if (half > 0.0f) focal = longer * 0.5f / half;
        }
        // LayerOrbit::boxToSurface with a depth scale of 1.
        const float cx = box_w * 0.5f, cy = box_h * 0.5f;
        const float sx = dest_w / box_w;
        const float sy = dest_h / box_h;
        const V3 xRow{focal * right.x - cx * back.x, focal * right.y - cx * back.y,
                      focal * right.z - cx * back.z};
        const V3 yRow{focal * down.x - cy * back.x, focal * down.y - cy * back.y,
                      focal * down.z - cy * back.z};
        const V3 wRow{-back.x, -back.y, -back.z};
        float m[16] = {};
        m[0] = sx * xRow.x;
        m[1] = sx * xRow.y;
        m[2] = sx * xRow.z;
        m[3] = -sx * dot(xRow, eye);
        m[4] = sy * yRow.x;
        m[5] = sy * yRow.y;
        m[6] = sy * yRow.z;
        m[7] = -sy * dot(yRow, eye);
        m[10] = 1.0f;
        m[12] = wRow.x;
        m[13] = wRow.y;
        m[14] = wRow.z;
        m[15] = -dot(wRow, eye);
        std::copy(m, m + 16, out);
        return XT_STATUS_OK;
    });
}

XT_CAPI xt_status xt_world_layer_unproject(const float box_to_dest[16], float depth_scale, float dest_x,
                                           float dest_y, float origin_out[3], float direction_out[3]) {
    XT_CAPI_GUARD("xt_world_layer_unproject", {
        if (box_to_dest == nullptr || origin_out == nullptr || direction_out == nullptr) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_world_layer_unproject: a pointer is NULL");
        }
        if (!finite(depth_scale) || depth_scale < 0.0f || !finite(dest_x) || !finite(dest_y)) {
            return fail(XT_STATUS_INVALID_ARGUMENT,
                        "xt_world_layer_unproject: depth_scale must be >= 0 and finite, the pixel finite");
        }
        const float* m = box_to_dest;
        for (int i = 0; i < 16; ++i) {
            if (!finite(m[i])) {
                return fail(XT_STATUS_INVALID_ARGUMENT, "xt_world_layer_unproject: the matrix is not finite");
            }
        }
        // In the scaled space the matrix sees (z already multiplied by the depth scale) the points of the
        // pixel satisfy X - px W = 0 and Y - py W = 0: two planes whose line is the ray.
        const V3 rx{m[0], m[1], m[2]}, ry{m[4], m[5], m[6]}, rw{m[12], m[13], m[14]};
        const float tx = m[3], ty = m[7], tw = m[15];
        const V3 a{rx.x - dest_x * rw.x, rx.y - dest_x * rw.y, rx.z - dest_x * rw.z};
        const V3 b{ry.x - dest_y * rw.x, ry.y - dest_y * rw.y, ry.z - dest_y * rw.z};
        const float ad = tx - dest_x * tw, bd = ty - dest_y * tw;
        V3 dir = cross(a, b);
        const float rwLen = std::sqrt(dot(rw, rw));
        const float scale = std::max({std::sqrt(dot(a, a)) * std::sqrt(dot(b, b)), 1e-30f});
        if (!(std::sqrt(dot(dir, dir)) > 1e-7f * scale)) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_world_layer_unproject: no ray through that pixel");
        }
        V3 origin{};
        const float det = dot(rx, cross(ry, rw));  // the eye: X = Y = W = 0
        const float rowScale = std::sqrt(dot(rx, rx)) * std::sqrt(dot(ry, ry)) * rwLen;
        if (rwLen > 0.0f && std::fabs(det) > 1e-6f * rowScale) {
            // Perspective: from the eye, toward growing W, scaled so that its component along the view
            // axis is 1 (LayerOrbit::ray's direction).
            const V3 c0 = cross(ry, rw), c1 = cross(rw, rx), c2 = cross(rx, ry);
            const float inv = -1.0f / det;
            origin = V3{(tx * c0.x + ty * c1.x + tw * c2.x) * inv, (tx * c0.y + ty * c1.y + tw * c2.y) * inv,
                        (tx * c0.z + ty * c1.z + tw * c2.z) * inv};
            const float along = dot(dir, rw) / rwLen;
            if (!(std::fabs(along) > 0.0f)) {
                return fail(XT_STATUS_INVALID_ARGUMENT,
                            "xt_world_layer_unproject: no ray through that pixel");
            }
            dir = V3{dir.x / along, dir.y / along, dir.z / along};
        } else {
            // Orthographic (W does not depend on the point): along -z, from far in front.
            if (dir.z > 0.0f) dir = V3{-dir.x, -dir.y, -dir.z};
            if (!(std::fabs(dir.z) > 1e-7f * std::sqrt(dot(dir, dir)))) {
                return fail(XT_STATUS_INVALID_ARGUMENT,
                            "xt_world_layer_unproject: the ray never crosses the box");
            }
            const float len = std::sqrt(dot(dir, dir));
            dir = V3{dir.x / len, dir.y / len, dir.z / len};
            // The point of the line at z = kOrthoOriginZ: a.p = -ad, b.p = -bd with p.z fixed.
            const float z = kOrthoOriginZ;
            const float e = -ad - a.z * z, f = -bd - b.z * z;
            const float d2 = a.x * b.y - a.y * b.x;
            if (std::fabs(d2) > 1e-12f) {
                origin = V3{(e * b.y - a.y * f) / d2, (a.x * f - e * b.x) / d2, z};
            } else {
                // The line is not crossed by that plane cleanly: walk from its closest point instead.
                const V3 n = cross(a, b);
                const float nn = dot(n, n);
                const V3 bn = cross(b, n), na = cross(n, a);
                const V3 p0{(bn.x * -ad + na.x * -bd) / nn, (bn.y * -ad + na.y * -bd) / nn,
                            (bn.z * -ad + na.z * -bd) / nn};
                const float t = (z - p0.z) / dir.z;
                origin = V3{p0.x + dir.x * t, p0.y + dir.y * t, z};
            }
        }
        // Back to root-box coordinates: the scene's z is the box's z times the depth scale
        // (LayerOrbit::ray's clamp keeps a flat view's ray finite).
        const float depth = std::max(depth_scale, 1.0e-3f);
        origin.z /= depth;
        dir.z /= depth;
        if (!finite(origin.x) || !finite(origin.y) || !finite(origin.z) || !finite(dir.x) || !finite(dir.y) ||
            !finite(dir.z)) {
            return fail(XT_STATUS_INVALID_ARGUMENT, "xt_world_layer_unproject: no ray through that pixel");
        }
        origin_out[0] = origin.x;
        origin_out[1] = origin.y;
        origin_out[2] = origin.z;
        direction_out[0] = dir.x;
        direction_out[1] = dir.y;
        direction_out[2] = dir.z;
        return XT_STATUS_OK;
    });
}

}  // extern "C"
