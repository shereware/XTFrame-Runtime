#pragma once

#include <cstdint>
#include <string_view>

#include "style_decode.h"  // core/src: the StyleProp numbers (T-413).
#include "xt/compile/style_model.h"
#include "xt/node.h"  // ConstraintKind (T-243).
#include "xt/property.h"

/// CSS name <-> PropId/StyleProp table, enum value tables, initial values and the `inherited`
/// flag. See docs/design/05-style-and-css.md §5.4, docs/tasks/T-22.md.

namespace xt::compile {

/// StyleProp numbers, docs/design/05-style-and-css.md §5.4. Not animatable (PropId 0-159 covers
/// the animatable range instead; see core/include/xt/property.h). T-413: the one table is core's
/// (core/src/style_decode.h, which the runtime decodes with); the compiler names the same numbers
/// through it.
namespace styleprop {
using namespace xt::style_decode::styleprop;
/// StyleProps share IrDecl's number space with the animatable PropIds. The shape-modifier /
/// parametric-shape / rig PropIds take 208-255 (T-225b, T-243), so StyleProps fill 160-207
/// (kLastLow) and continue from kFirstHigh (256), below the effect PropIds (0x8000).
inline constexpr uint16_t kLastLow = ClipPath;
static_assert(kLastLow < xt::prop::ModTrimStart, "StyleProp numbers collide with modifier PropIds");
inline constexpr uint16_t kFirstHigh = 256;
static_assert(kFirstHigh > xt::prop::ConstraintEnabled, "StyleProp numbers collide with rig PropIds");
/// Highest StyleProp number.
inline constexpr uint16_t kLast = HitSlopLeft;
static_assert(kLast == xt::kStylePropLast, "update kStylePropLast (ir_pod.h) and 02 §2.12");
static_assert(kLast < xt::prop::kEffectPropBase, "StyleProp numbers collide with effect PropIds");
}  // namespace styleprop

/// T-190 (03 §3.12.6, 11 §11.9): true for a vector-content property (PropId 32-51: `d`, `fill`,
/// `stroke`, `path-trim`, ...). These only apply to the Path/Group nodes of an inline `<svg>` and
/// are accepted only in `@timeline` keys; everywhere else they are dropped with a warning.
inline bool isVectorProp(uint16_t prop) {
    return prop >= xt::prop::PathData && prop <= xt::prop::StrokeTaperEnd;
}

/// T-190/T-218: the node-level properties an inline `<svg>` scene node of `kind` (Path or Group)
/// takes from an `@timeline` key or a scene-target rule (`#Icon/Blade { ... }`): opacity, the
/// transform components and transform-origin, and -- on a Path -- `d`, fill/stroke color and
/// opacity, stroke-width, stroke-dashoffset and path-trim. T-225: a Shape takes the same minus `d`
/// plus its shape parameters (232-245); a Modifier only opacity and its parameters (208-231).
bool appliesToSvgNode(uint16_t prop, NodeKind kind);

/// T-225: appliesToSvgNode() for some inline `<svg>` node kind (Path, Shape or Modifier) -- what
/// a scene-target rule may declare, since its targets' kinds are only known at runtime.
bool appliesToAnySvgNode(uint16_t prop);

/// A synthetic prop number for CSS names that are not a single value (shorthands, `transform`,
/// `transform-origin`): findProp() returns a PropInfo with this prop and kind == Shorthand; the
/// caller must not write a StyleDecl with this number.
inline constexpr uint16_t kShorthandProp = 0xFFFE;

/// `animation`'s direction/fill-mode/play-state keyword sets. Not part of 05.4's numbered table;
/// chosen conservatively in CSS keyword-list order (see T-22 report).
enum class AnimDirection : uint8_t { Normal = 0, Reverse = 1, Alternate = 2, AlternateReverse = 3 };
enum class AnimFillMode : uint8_t { None = 0, Forwards = 1, Backwards = 2, Both = 3 };
enum class AnimPlayState : uint8_t { Running = 0, Paused = 1 };

struct PropInfo {
    const char* cssName;
    uint16_t prop;
    StyleValueKind kind;
    bool inherited;
    bool animatable;
};

/// T-225 (31 §31.6.1): true for an entry of the modifier / parametric-shape name table
/// (`trim-end`, `copies`, `r`, `points`, ...; PropId 208-231 and 232-245).
bool isSvgNodePropInfo(const PropInfo& info);

/// isVectorProp() for a table entry, including the `path-trim` shorthand and (T-225) the
/// modifier / parametric-shape parameters.
inline bool isVectorPropInfo(const PropInfo& info) {
    return isVectorProp(info.prop) || std::string_view(info.cssName) == "path-trim" ||
           isSvgNodePropInfo(info);
}

/// Looks up a CSS property/shorthand name (e.g. "background-color", "margin"), then (T-225) the
/// modifier / parametric-shape names. Returns nullptr if unknown.
const PropInfo* findProp(std::string_view cssName);

/// T-225: the modifier / parametric-shape name-table entry of `prop` (PropId 208-231, 232-245),
/// nullptr if none. findPropById() does not look there (svg-only names).
const PropInfo* findSvgNodePropById(uint16_t prop);

/// T-225: the PropId an inline `<svg>` node of `kind` takes for `prop` -- CSS `width`/`height` are
/// a Shape's ShapeW/ShapeH (31 §31.6.1); T-243: on a Constraint `enabled` is ConstraintEnabled and
/// `strength` is ConstraintParam1 when `constraintKind` is Spring (IkStrength otherwise); every
/// other prop is returned unchanged.
uint16_t svgNodeProp(uint16_t prop, NodeKind kind, ConstraintKind constraintKind = ConstraintKind::Ik);

/// Looks up a resolved (non-shorthand) prop number's metadata. Returns nullptr if not a known
/// PropId/StyleProp.
const PropInfo* findPropById(uint16_t prop);

/// Enum keyword name for `prop`'s `value`. Returns nullptr if `prop` isn't an Enum property or
/// `value` has no keyword (e.g. an arbitrary font-weight number).
const char* enumName(uint16_t prop, uint32_t value);

/// Enum keyword -> value for `prop`. Returns false if `prop` isn't an Enum property or `name`
/// isn't one of its keywords.
bool enumValue(uint16_t prop, std::string_view name, uint32_t& out);

/// The property's initial value, docs/design/05-style-and-css.md §5.4. XT_ASSERT if `prop` is not
/// a known PropId/StyleProp.
StyleValue initialValue(uint16_t prop);

}  // namespace xt::compile
