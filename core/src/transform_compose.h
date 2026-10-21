#pragma once

#include <cstddef>
#include <cstdint>

#include "xt/types.h"

/// Arbitrary-order CSS `transform` sequences: compose the functions into one 4x4 affine matrix,
/// then decompose that matrix back into the fixed component set the scene graph stores
/// (translate/rotateX/rotateY/rotate/skew/scale). See docs/design/05-style-and-css.md §5.5 and
/// docs/design/03-scene-and-properties.md §3.3, docs/tasks/T-120.md.
///
/// A private (core/src) header, shared by the compiler's `.xts` value parser
/// (compile/src/xts/xts_values.cpp) and the runtime mini CSS parser (core/src/mini_css.cpp) so
/// both paths agree bit for bit. Pure functions: no allocation, no diagnostics, no I/O.
///
/// Callers only reach this after their own fast path has rejected the sequence: a sequence that
/// already follows 05.5's fixed order with each category at most once maps straight onto the
/// PropIds and never composes a matrix (its output must stay byte-identical to before T-120).

namespace xt {

/// One transform function in source order, with its arguments already parsed into px/degrees.
struct TransformFn {
    enum class Kind : uint8_t {
        Translate,  ///< a = x px, b = y px, c = z px.
        RotateX,    ///< a = degrees.
        RotateY,    ///< a = degrees.
        RotateZ,    ///< a = degrees (CSS `rotate`).
        Skew,       ///< a = skewX degrees, b = skewY degrees (CSS `skew`/`skewX`/`skewY`).
        Scale,      ///< a = x factor, b = y factor.
    };
    Kind kind = Kind::Translate;
    float a = 0;
    float b = 0;
    float c = 0;
};

/// The component set 03.3's NodeCommon (plus 19.3's 3D components) stores, i.e. what the
/// `transform` shorthand expands into. Composing them in 03.3 order reproduces the source matrix:
/// `T(translate) * Rz(rotation) * Ry(rotationY) * Rx(rotationX) * Sk(skew) * S(scale)`, which is
/// exactly core/src/world_transform.cpp's composeLocal3() with the anchor left out.
struct TransformComponents {
    float translateX = 0;
    float translateY = 0;
    float translateZ = 0;
    float rotationX = 0;  ///< Degrees.
    float rotationY = 0;  ///< Degrees.
    float rotation = 0;   ///< Degrees, around Z.
    float skewX = 0;      ///< Degrees.
    float skewY = 0;      ///< Degrees (always 0: the decomposition only ever produces an X skew).
    float scaleX = 1;
    float scaleY = 1;
};

/// Why a decomposition failed.
enum class TransformDecomposeStatus : uint8_t {
    Ok,       ///< `out` is valid.
    Shear3D,  ///< 05.5: the matrix needs a 3D shear or a z scale, which 03.3 cannot represent.
};

/// Composes `count` functions with CSS semantics: `M = F1 * F2 * ... * Fn`, so the function
/// written last is the one applied first to a point. An empty sequence yields the identity.
Mat4 composeTransformSequence(const TransformFn* fns, size_t count);

/// Composes `c` back into a matrix, in 03.3 order. Inverse of decomposeTransform().
Mat4 composeTransformComponents(const TransformComponents& c);

/// Decomposes an affine 4x4 into 03.3's components (05.5's QR-style algorithm). Returns
/// Shear3D and leaves `out` untouched when the third axis is not a unit vector orthogonal to the
/// other two (tolerance 1e-3), i.e. when the matrix needs a 3D shear or a z scale.
TransformDecomposeStatus decomposeTransform(const Mat4& m, TransformComponents& out);

/// composeTransformSequence() followed by decomposeTransform().
TransformDecomposeStatus decomposeTransformSequence(const TransformFn* fns, size_t count,
                                                    TransformComponents& out);

/// T-322: a 2D CSS `matrix(a, b, c, d, e, f)` as the equivalent function sequence
/// `translate(e, f) rotate(r) skewX(k) scale(sx, sy)` (written into `out[0..3]`), so it joins 05.5's
/// compose/decompose path like any other function. Returns false (and leaves `out` untouched) when
/// the matrix is singular (|ad - bc| below 1e-6) or not finite: such a matrix has no decomposition.
bool decomposeMatrix2D(float a, float b, float c, float d, float e, float f, TransformFn out[4]);

/// 05.5's warning condition: a decomposed skew beyond +/-89 degrees (a near-degenerate matrix
/// whose components animate badly). Not an error.
bool transformSkewOutOfRange(const TransformComponents& c);

}  // namespace xt
