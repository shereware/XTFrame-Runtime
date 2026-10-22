#pragma once

#include <cstdint>
#include <vector>

#include "xt/compile/ir_writer.h"
#include "xt/compile/style_model.h"

/// T-176: `calc()` programs -> the `CALC` chunk, `@media` blocks -> the `MDIA` chunk (Docs/design/
/// 05-style-and-css.md §5.9.2-5.9.3, 06-layout.md §6.4, 12-compiler.md §12.7).

namespace xt::compile {

/// Appends `ops` (a parsed `calc()`, already type-checked by the parser) to `writer`'s `CALC`
/// chunk and returns the program index a kDeclCalc declaration stores in value slot 0.
uint32_t emitCalc(const std::vector<CalcOpDecl>& ops, IrWriter& writer);

/// T-330 (05 §5.9.8): a program holding `min()` / `max()` / `clamp()` (a Min or Max op) whose every
/// term is a px length or a plain number folds to its px result at compile time; returns false
/// (and leaves `px` alone) for anything else -- a %, em, vw, safe-*, uiscale or var() term, or a
/// plain `calc()` without Min/Max (written to `CALC` as before).
bool foldCalcToPx(const std::vector<CalcOpDecl>& ops, float& px);

/// Writes the `MDIA` chunk: every `@media` block of `media` plus `ruleMap` (STYL rule index ->
/// block index, in ascending rule order). Writes nothing when `ruleMap` is empty.
void emitMedia(const std::vector<MediaDef>& media, const std::vector<IrMediaRule>& ruleMap, IrWriter& writer);

}  // namespace xt::compile
