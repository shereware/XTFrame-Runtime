#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "xt/assert.h"

/// Basic value types (ids, Vec2/Color/Rect/Mat2x3), Status/Result error handling.
/// 見 Docs/design/03-scene-and-properties.md §3.1, §3.3, Docs/design/14-coding-standards.md §14.3

namespace xt {

/// Handle to a scene/element node in a NodeArena.
using NodeId = uint32_t;
/// Sentinel NodeId meaning "no node" / not found.
inline constexpr NodeId kInvalidNode = 0xFFFFFFFFu;
/// Sentinel NodeId meaning "the element's own scene node" (used where a timeline target may be
/// either a named scene node or the element itself).
inline constexpr NodeId kSelfNode = 0xFFFFFFFEu;
/// Id of a scene/element property (see Docs/design/03-scene-and-properties.md).
using PropId = uint16_t;
/// Id of an interned string, either in the IR's immutable string table or a Document's runtime
/// string pool (see kRuntimeStringBase below).
using StringId = uint32_t;
/// Sentinel StringId meaning "no string" / not found.
inline constexpr StringId kInvalidString = 0xFFFFFFFFu;
/// First StringId of a Document's runtime string pool (Docs/design/13-api.md §13.2's intern(),
/// Docs/tasks/T-35.md "執行時字串池"): ids below it index the immutable IR string table, ids at or
/// above it that Document's pool. kInvalidString is above it too and is always rejected first.
inline constexpr StringId kRuntimeStringBase = 0x80000000u;
/// First StringId of a Document's node-owned runtime text table (T-270a): the text a host or a
/// `bind:text` binding set on a Text node (ElementRef::setText) lives in one slot per Text node,
/// reused on every change and released with the node, instead of being interned. Ids in
/// [kRuntimeTextBase, kInvalidString) index that table, so the runtime string pool above ends
/// below it. Resolved by the same accessors as every other StringId (Document::string).
inline constexpr StringId kRuntimeTextBase = 0xC0000000u;
/// Id of an image/font/etc. asset referenced by the IR.
using AssetId = uint32_t;
/// Sentinel AssetId meaning "no asset" / not found.
inline constexpr AssetId kInvalidAsset = 0xFFFFFFFFu;
/// Id of a scene (Lottie-derived vector scene graph) embedded in the IR.
using SceneId = uint32_t;
/// Sentinel SceneId meaning "no scene" / not found.
inline constexpr SceneId kInvalidScene = 0xFFFFFFFFu;
/// Id of a keyframe timeline (either compiled into the IR or created at runtime).
using TimelineId = uint32_t;
/// Sentinel TimelineId meaning "no timeline" / not found.
inline constexpr TimelineId kInvalidTimeline = 0xFFFFFFFFu;
/// Id of a font registered with a FontProvider/TextShaper.
using FontId = uint32_t;
/// Sentinel FontId meaning "no font" / not found.
inline constexpr FontId kInvalidFont = 0xFFFFFFFFu;

/// 2D point/vector.
struct Vec2 {
    float x = 0;  ///< X coordinate.
    float y = 0;  ///< Y coordinate.
};

/// Componentwise addition.
inline Vec2 operator+(Vec2 lhs, Vec2 rhs) { return Vec2{lhs.x + rhs.x, lhs.y + rhs.y}; }
/// Componentwise subtraction.
inline Vec2 operator-(Vec2 lhs, Vec2 rhs) { return Vec2{lhs.x - rhs.x, lhs.y - rhs.y}; }
/// Negation.
inline Vec2 operator-(Vec2 v) { return Vec2{-v.x, -v.y}; }
/// Scales both components by s.
inline Vec2 operator*(Vec2 v, float s) { return Vec2{v.x * s, v.y * s}; }
/// Scales both components by s.
inline Vec2 operator*(float s, Vec2 v) { return Vec2{v.x * s, v.y * s}; }
/// Divides both components by s.
inline Vec2 operator/(Vec2 v, float s) { return Vec2{v.x / s, v.y / s}; }
/// Componentwise addition, in place.
inline Vec2& operator+=(Vec2& lhs, Vec2 rhs) {
    lhs.x += rhs.x;
    lhs.y += rhs.y;
    return lhs;
}
/// Componentwise subtraction, in place.
inline Vec2& operator-=(Vec2& lhs, Vec2 rhs) {
    lhs.x -= rhs.x;
    lhs.y -= rhs.y;
    return lhs;
}
/// Scales both components by s, in place.
inline Vec2& operator*=(Vec2& lhs, float s) {
    lhs.x *= s;
    lhs.y *= s;
    return lhs;
}
/// Componentwise equality.
inline bool operator==(Vec2 lhs, Vec2 rhs) { return lhs.x == rhs.x && lhs.y == rhs.y; }
/// Componentwise inequality.
inline bool operator!=(Vec2 lhs, Vec2 rhs) { return !(lhs == rhs); }

/// RGBA color, components in [0, 1] by convention (not enforced).
struct Color {
    float r = 0;  ///< Red.
    float g = 0;  ///< Green.
    float b = 0;  ///< Blue.
    float a = 1;  ///< Alpha.
};

/// Componentwise equality.
inline bool operator==(const Color& lhs, const Color& rhs) {
    return lhs.r == rhs.r && lhs.g == rhs.g && lhs.b == rhs.b && lhs.a == rhs.a;
}
/// Componentwise inequality.
inline bool operator!=(const Color& lhs, const Color& rhs) { return !(lhs == rhs); }

/// Axis-aligned rectangle, (x, y) is top-left in a y-down coordinate system.
struct Rect {
    float x = 0;  ///< Left edge.
    float y = 0;  ///< Top edge.
    float w = 0;  ///< Width.
    float h = 0;  ///< Height.
};

/// Componentwise equality.
inline bool operator==(const Rect& lhs, const Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.w == rhs.w && lhs.h == rhs.h;
}
/// Componentwise inequality.
inline bool operator!=(const Rect& lhs, const Rect& rhs) { return !(lhs == rhs); }

/// True if p is inside rect, left/top inclusive, right/bottom exclusive.
inline bool contains(const Rect& rect, Vec2 p) {
    return p.x >= rect.x && p.x < rect.x + rect.w && p.y >= rect.y && p.y < rect.y + rect.h;
}

/// Intersection of two rects. No overlap (including merely touching) returns a Rect with w=h=0,
/// x/y set to the max of each rect's origin.
inline Rect intersect(const Rect& a, const Rect& b) {
    float x0 = a.x > b.x ? a.x : b.x;
    float y0 = a.y > b.y ? a.y : b.y;
    float ax1 = a.x + a.w;
    float ay1 = a.y + a.h;
    float bx1 = b.x + b.w;
    float by1 = b.y + b.h;
    float x1 = ax1 < bx1 ? ax1 : bx1;
    float y1 = ay1 < by1 ? ay1 : by1;
    if (x1 <= x0 || y1 <= y0) {
        return Rect{x0, y0, 0, 0};
    }
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// Union of two rects' bounding box. Either rect with w<=0||h<=0 is treated as empty and the other
/// rect is returned unchanged.
inline Rect unite(const Rect& a, const Rect& b) {
    bool aEmpty = a.w <= 0 || a.h <= 0;
    bool bEmpty = b.w <= 0 || b.h <= 0;
    if (aEmpty) {
        return b;
    }
    if (bEmpty) {
        return a;
    }
    float x0 = a.x < b.x ? a.x : b.x;
    float y0 = a.y < b.y ? a.y : b.y;
    float ax1 = a.x + a.w;
    float ay1 = a.y + a.h;
    float bx1 = b.x + b.w;
    float by1 = b.y + b.h;
    float x1 = ax1 > bx1 ? ax1 : bx1;
    float y1 = ay1 > by1 ? ay1 : by1;
    return Rect{x0, y0, x1 - x0, y1 - y0};
}

/// 2x3 affine transform matrix: [a c e; b d f]. apply(m, p) = (a*px + c*py + e, b*px + d*py + f).
struct Mat2x3 {
    float a = 1;  ///< Linear x-scale/rotation term.
    float b = 0;  ///< Linear y-shear term.
    float c = 0;  ///< Linear x-shear term.
    float d = 1;  ///< Linear y-scale/rotation term.
    float e = 0;  ///< X translation.
    float f = 0;  ///< Y translation.
};

/// Matrix product lhs * rhs (applying the result to a point is equivalent to applying rhs then
/// lhs).
Mat2x3 mul(const Mat2x3& lhs, const Mat2x3& rhs);
/// Transforms a point by m (includes translation).
Vec2 apply(const Mat2x3& m, Vec2 p);
/// Transforms a direction vector by m (ignores translation).
Vec2 applyVector(const Mat2x3& m, Vec2 v);
/// Computes the inverse of m into out. Returns false (out left unchanged) if m is singular.
bool invert(const Mat2x3& m, Mat2x3& out);
/// Returns the determinant of m's linear part (a*d - b*c).
float determinant(const Mat2x3& m);
/// Returns a matrix that translates by t.
Mat2x3 translation(Vec2 t);
/// Returns a matrix that scales by s.
Mat2x3 scaling(Vec2 s);
/// Returns a matrix that rotates by deg degrees (clockwise, since y points down).
Mat2x3 rotationDeg(float deg);
/// Returns a matrix that skews by skewX degrees along an axis rotated skewAxis degrees.
Mat2x3 skewDeg(float skewX, float skewAxis);
/// Composes translate/rotate/skew/scale around anchor into a single matrix, in the order used by
/// scene node transforms (see Docs/design/03-scene-and-properties.md).
Mat2x3 composeTRSS(Vec2 translate, float rotDeg, Vec2 skew, Vec2 scale, Vec2 anchor);
/// Returns the axis-aligned bounding box of rect after being transformed by m.
Rect transformBounds(const Mat2x3& m, const Rect& rect);
/// True if every component of lhs and rhs differs by no more than eps.
bool approxEqual(const Mat2x3& lhs, const Mat2x3& rhs, float eps = 1e-5f);

/// 3D point/vector.
struct Vec3 {
    float x = 0;  ///< X coordinate.
    float y = 0;  ///< Y coordinate.
    float z = 0;  ///< Z coordinate.
};

/// 4x4 matrix, row-major: `m[row * 4 + col]`. Points are column vectors, `p' = M * p`;
/// translation lives in `m[0*4+3], m[1*4+3], m[2*4+3]`. This layout maps 1:1 onto CryEngine's
/// `Matrix44_tpl::mRC` naming and its column-vector `operator*(Matrix, Vec3)` /
/// `TransformPoint` / `TransformVector` split, so a host embedding XTFrame can copy elements
/// without transposing. Intentional differences from CryEngine: angles taken by the functions
/// below are degrees (CryMath uses radians), and XTFrame's axes are x right, y down, +z toward
/// the viewer with the camera looking toward -z, unlike CryEngine's Z-up / Y-forward world;
/// converting a camera requires an axis remap, not just a transpose.
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

/// 3x3 matrix, row-major; used for 2D homographies (perspective quad mappings).
struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

/// Returns the 4x4 identity matrix.
Mat4 mat4Identity();
/// Matrix product a * b.
Mat4 mat4Mul(const Mat4& a, const Mat4& b);
/// Embeds a 2D affine transform [a c e; b d f] into a 4x4 matrix at z = 0.
Mat4 mat4FromMat2x3(const Mat2x3& t);
/// Returns a matrix that translates by t.
Mat4 mat4Translate(Vec3 t);
/// Returns a matrix that scales by s.
Mat4 mat4Scale(Vec3 s);
/// Returns a matrix that rotates by deg degrees around the X axis (right-handed).
Mat4 mat4RotateX(float deg);
/// Returns a matrix that rotates by deg degrees around the Y axis (right-handed).
Mat4 mat4RotateY(float deg);
/// Returns a matrix that rotates by deg degrees around the Z axis (right-handed; matches
/// Mat2x3's rotationDeg direction in a y-down coordinate system).
Mat4 mat4RotateZ(float deg);
/// View matrix moving eye to the origin, looking toward -z (OpenGL gluLookAt convention).
Mat4 mat4LookAt(Vec3 eye, Vec3 target, Vec3 up);
/// Perspective projection matrix (gluPerspective convention). fovYDeg is the vertical field of
/// view in degrees; NDC z in [-1, 1].
Mat4 mat4Perspective(float fovYDeg, float aspect, float nearZ, float farZ);
/// Orthographic projection matrix (glOrtho convention).
Mat4 mat4Ortho(float l, float r, float b, float t, float nearZ, float farZ);
/// General 4x4 matrix inverse (cofactor method). Returns false (out left unchanged) if
/// |det| < 1e-12.
bool mat4Invert(const Mat4& m, Mat4& out);
/// Transforms a point by m: multiplies (p, 1) and takes xyz, without dividing by w.
Vec3 mat4Transform(const Mat4& m, Vec3 p);
/// Transforms a direction vector by m's upper-left 3x3 only (ignores translation).
Vec3 mat4TransformVector(const Mat4& m, Vec3 v);
/// Transforms a point by m with the homogeneous divide. If wOut is non-null, receives w (0 and
/// (0,0,0) returned when w <= 1e-6).
Vec3 mat4Project(const Mat4& m, Vec3 p, float* wOut);
/// Computes the 3x3 homography (DLT) mapping src's 4 points to dst's 4 points. Returns false
/// (out left unchanged) if the points are degenerate.
bool homographyFromQuad(const Vec2 src[4], const Vec2 dst[4], Mat3& out);
/// Transforms p by h with the homogeneous divide; returns p unchanged if w <= 1e-6.
Vec2 mat3Apply(const Mat3& h, Vec2 p);

/// Result/error status for fallible operations.
enum class Status : uint8_t {
    kOk = 0,           ///< Success.
    kInvalidArgument,  ///< A caller-supplied argument was invalid.
    kInvalidIr,        ///< The IR blob is malformed or failed validation.
    kNotFound,         ///< The requested id/name/entry does not exist.
    kOutOfRange,       ///< An index or offset was out of bounds.
    kUnsupported,      ///< The operation is not supported in this context.
    kIoError,          ///< A file/stream read or write failed.
    kInternal,         ///< An internal invariant was violated.
    /// T-200 (22.10): the new content does not match the old structurally (an id the old state
    /// refers to is gone), so the old state could not be carried over.
    kIncompatible
};

/// Returns the enum name (e.g. "kOk"); unknown values return "?".
inline const char* statusName(Status status) {
    switch (status) {
        case Status::kOk:
            return "kOk";
        case Status::kInvalidArgument:
            return "kInvalidArgument";
        case Status::kInvalidIr:
            return "kInvalidIr";
        case Status::kNotFound:
            return "kNotFound";
        case Status::kOutOfRange:
            return "kOutOfRange";
        case Status::kUnsupported:
            return "kUnsupported";
        case Status::kIoError:
            return "kIoError";
        case Status::kInternal:
            return "kInternal";
        case Status::kIncompatible:
            return "kIncompatible";
    }
    return "?";
}

/// Either a value of type T (status == kOk) or an error Status.
template <class T>
class Result {
public:
    /// Builds a successful Result holding v.
    static Result ok(T v) {
        Result r;
        r.m_status = Status::kOk;
        r.m_value = std::move(v);
        r.m_hasValue = true;
        return r;
    }
    /// Builds a failed Result with error status s. s must not be Status::kOk.
    static Result error(Status s) {
        XT_ASSERT(s != Status::kOk);
        Result r;
        r.m_status = s;
        return r;
    }
    /// True if this Result holds a value (status() == Status::kOk).
    bool ok() const { return m_status == Status::kOk; }
    /// Returns the error/success status.
    Status status() const { return m_status; }
    /// Returns the held value. Only valid to call when ok() is true.
    T& value() {
        XT_ASSERT(m_hasValue);
        return m_value;
    }
    /// Returns the held value. Only valid to call when ok() is true.
    const T& value() const {
        XT_ASSERT(m_hasValue);
        return m_value;
    }
    /// Returns the held value if ok(), otherwise fallback.
    T valueOr(T fallback) const { return m_hasValue ? m_value : fallback; }

private:
    Status m_status = Status::kInternal;
    bool m_hasValue = false;
    T m_value{};  // T 必須可預設建構；本專案所有 Result<T> 的 T 皆滿足
};

}  // namespace xt
