#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/style_model.h"
#include "xt/timeline.h"
#include "xts_lexer.h"

/// T-756 (Docs/design/05-style-and-css.md §5.6): the parser's half of the `animation-*`
/// longhands -- one declaration block's `animation` / `animation-*` declarations. T-778: there is
/// no compile-time cascade any more; each longhand is a style property the runtime cascades
/// (StyleProp 282-289), and a block only merges what it says itself.

namespace xt::compile {

/// One `animation` (longhand < 0) or `animation-*` declaration of a block, in source order.
struct AnimationPart {
    int longhand = -1;  ///< css_values::AnimationLonghand, or -1 for the shorthand.
    std::vector<Token> value;
    int line = 0;  ///< Reported line.
    bool important = false;
    /// How many StyleDecls the shorthand produced (0 when it was invalid); longhands produce none.
    size_t emitted = 0;
};

/// When `parts` holds a longhand after the block's last valid shorthand (the shorthand sets all
/// eight, so earlier longhands change nothing): with that shorthand, its AnimationList declaration
/// takes the longhands (a later declaration winning) and keeps every item (`none` names and
/// iteration counts of 0 included -- the runtime leaves them out after the cascade); without one,
/// one declaration of the given longhands is appended -- an AnimationList when all eight are given,
/// else an AnimLonghands value. A longhand whose value holds `var()` is appended as a VarSubst
/// declaration of its own (prop = its StyleProp), after the rest. An invalid longhand is reported
/// and skipped, as CSS ignores an invalid declaration. `!important` is taken from the last one.
void mergeAnimationLonghands(const std::vector<AnimationPart>& parts, std::vector<StyleDecl>& decls,
                             std::string_view fileName, DiagnosticSink& sink,
                             std::vector<EasingPoint>* easingPoints);

/// T-778: for an `animation-*` / `transition-*` longhand StyleProp (282-293), the declaration of
/// its initial value (a VarSubst whose var() is undefined, CSS's "invalid at computed-value
/// time"): an AnimLonghands / TransLonghands value with that one longhand given and no items.
/// False for any other prop.
bool longhandInitialDecl(uint16_t prop, StyleDecl& out);

}  // namespace xt::compile
