#include "camera.h"

#include <cmath>

/// camera.h's implementation. See docs/design/19-3d-space.md §19.4.

namespace xt {

namespace {
constexpr float kRadToDeg = 57.29577951308232f;

Vec3 vecSub(Vec3 a, Vec3 b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 vecCross(Vec3 a, Vec3 b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float vecDot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 vecNormalize(Vec3 v) {
    float len = std::sqrt(vecDot(v, v));
    if (len < 1e-12f) {
        return v;
    }
    return Vec3{v.x / len, v.y / len, v.z / len};
}

/// 19.4 step 2: the y-down camera frame. b = camera's z axis (toward the viewer, eye - target),
/// s = right, d = down. Built directly from the frame's rows, not gluLookAt, which mirrors an
/// axis in a y-down world.
Mat4 cameraFrameView(Vec3 eye, Vec3 target) {
    Vec3 b = vecNormalize(vecSub(eye, target));
    Vec3 upCrossB = vecCross(Vec3{0, 1, 0}, b);
    Vec3 s = std::sqrt(vecDot(upCrossB, upCrossB)) < 1e-6f ? Vec3{1, 0, 0} : vecNormalize(upCrossB);
    Vec3 d = vecCross(b, s);
    Mat4 out;
    // clang-format off
    out.m[0] = s.x; out.m[1] = s.y; out.m[2] = s.z;  out.m[3]  = -vecDot(s, eye);
    out.m[4] = d.x; out.m[5] = d.y; out.m[6] = d.z;  out.m[7]  = -vecDot(d, eye);
    out.m[8] = b.x; out.m[9] = b.y; out.m[10] = b.z; out.m[11] = -vecDot(b, eye);
    out.m[12] = 0;  out.m[13] = 0;  out.m[14] = 0;   out.m[15] = 1;
    // clang-format on
    return out;
}

/// 19.4 step 4: NDC -> the root's 2D box coordinates. No y flip: the view space is already
/// y-down.
Mat4 ndcToBoxMatrix(float w, float h) {
    return mat4Mul(mat4Translate(Vec3{w * 0.5f, h * 0.5f, 0}), mat4Scale(Vec3{w * 0.5f, h * 0.5f, 1}));
}
}  // namespace

CameraSetup computeCameraViewProj(const NodeArena& arena, NodeId cameraNode, Rect box) {
    const NodeCommon& c = arena.common(cameraNode);
    const CameraData& cam = arena.camera(cameraNode);
    Vec3 eye{c.translate.x, c.translate.y, c.translateZ};

    Mat4 view;
    if (cam.useTarget != 0) {
        view = cameraFrameView(eye, cam.target);
    } else {
        // 19.4 step 2: inverse(T(eye) * Rz(rotation) * Ry(rotationY) * Rx(rotationX)).
        Mat4 orient =
            mat4Mul(mat4RotateZ(c.rotation), mat4Mul(mat4RotateY(c.rotationY), mat4RotateX(c.rotationX)));
        Mat4 world = mat4Mul(mat4Translate(eye), orient);
        Mat4 inv;
        view = mat4Invert(world, inv) ? inv : mat4Identity();
    }

    float w = box.w;
    float h = box.h;
    Mat4 proj;
    if (cam.kind == 0) {  // Perspective.
        float zoom = cam.zoom != 0.0f ? cam.zoom : 1.0f;
        float fovY = 2.0f * std::atan((h * 0.5f) / zoom) * kRadToDeg;
        float aspect = h != 0.0f ? (w / h) : 1.0f;
        proj = mat4Perspective(fovY, aspect, cam.nearZ, cam.farZ);
    } else {  // Orthographic.
        proj = mat4Ortho(-w * 0.5f, w * 0.5f, -h * 0.5f, h * 0.5f, cam.nearZ, cam.farZ);
    }

    Mat4 ndcToBox = ndcToBoxMatrix(w, h);
    Mat4 viewProj = mat4Mul(ndcToBox, mat4Mul(proj, view));
    return CameraSetup{viewProj, false, view};
}

CameraSetup cssPerspectiveViewProj(float perspectivePx, Vec2 originPx) {
    if (perspectivePx <= 0.0f) {
        return CameraSetup{mat4Identity(), true, mat4Identity()};
    }
    Mat4 p = mat4Identity();
    p.m[14] = -1.0f / perspectivePx;  // row 3, col 2 (row-major m[row*4+col]).
    Mat4 toOrigin = mat4Translate(Vec3{-originPx.x, -originPx.y, 0});
    Mat4 fromOrigin = mat4Translate(Vec3{originPx.x, originPx.y, 0});
    Mat4 viewProj = mat4Mul(fromOrigin, mat4Mul(p, toOrigin));
    // §19.6: "CSS 預設攝影機時 view = I" -- there is no LookAt view for the CSS perspective root.
    return CameraSetup{viewProj, false, mat4Identity()};
}

CameraSetup defaultSceneViewProj(float width, float height) {
    Vec3 eye{width * 0.5f, height * 0.5f, width};
    Vec3 target{width * 0.5f, height * 0.5f, 0};
    Mat4 view = cameraFrameView(eye, target);
    float zoom = width != 0.0f ? width : 1.0f;
    float fovY = 2.0f * std::atan((height * 0.5f) / zoom) * kRadToDeg;
    float aspect = height != 0.0f ? (width / height) : 1.0f;
    Mat4 proj = mat4Perspective(fovY, aspect, 1.0f, 100000.0f);
    Mat4 ndcToBox = ndcToBoxMatrix(width, height);
    Mat4 viewProj = mat4Mul(ndcToBox, mat4Mul(proj, view));
    return CameraSetup{viewProj, false, view};
}

Rect spaceRootBoxOf(const NodeArena& arena, NodeId root) {
    if (arena.kind(root) == NodeKind::Element) {
        const LayoutBox& box = arena.element(root).box;
        return Rect{0, 0, box.width, box.height};
    }
    if (arena.kind(root) == NodeKind::Group) {
        NodeId p = arena.parent(root);
        if (p != kInvalidNode && arena.kind(p) == NodeKind::Element) {
            const ElementData& pdata = arena.element(p);
            if (pdata.scene != kInvalidScene && pdata.hasIntrinsic != 0) {
                return Rect{0, 0, pdata.intrinsic.x, pdata.intrinsic.y};
            }
        }
    }
    return Rect{};
}

CameraSetup rootCameraSetup(const NodeArena& arena, NodeId root) {
    NodeId cam = findCameraChild(arena, root);
    Rect box = spaceRootBoxOf(arena, root);
    if (cam != kInvalidNode) {
        return computeCameraViewProj(arena, cam, box);
    }
    if (arena.kind(root) == NodeKind::Element) {
        const ElementData& data = arena.element(root);
        return cssPerspectiveViewProj(data.perspectivePx, data.perspectiveOriginPx);
    }
    return defaultSceneViewProj(box.w, box.h);
}

NodeId nearestSpaceRootOrDocRoot(const NodeArena& arena, NodeId n) {
    NodeId top = n;
    NodeId cur = arena.parent(n);
    while (cur != kInvalidNode) {
        top = cur;
        if (arena.common(cur).isSpaceRoot != 0) {
            return cur;
        }
        cur = arena.parent(cur);
    }
    return top;
}

NodeId findCameraChild(const NodeArena& arena, NodeId root) {
    for (NodeId c = arena.firstChild(root); c != kInvalidNode; c = arena.nextSibling(c)) {
        if (arena.kind(c) == NodeKind::Camera && arena.common(c).visible != 0) {
            return c;
        }
        if (arena.kind(c) == NodeKind::Element && arena.element(c).cameraNode != kInvalidNode) {
            return arena.element(c).cameraNode;
        }
    }
    return kInvalidNode;
}

}  // namespace xt
