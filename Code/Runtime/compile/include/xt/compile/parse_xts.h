#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "xt/compile/style_model.h"

/// `.xts` stylesheet / declaration-list / single-value parse entry points. See
/// Docs/design/05-style-and-css.md §5.3-5.6, Docs/tasks/T-22.md.

namespace xt::compile {

class DiagnosticSink;

/// Parses `text` (a full `.xts` file, or a `<style>` block's contents) and appends rules /
/// \@keyframes / \@font-face to `out`. `fileName`/`lineOffset` are used for diagnostics (lineOffset
/// lets a `<style>` block embedded at some line N report 1-based positions relative to N: pass
/// N so the first line of `text` is reported as line N).
/// Returns false if any error-level diagnostic was raised (out may still have been partially
/// appended to; malformed rules are skipped via error recovery, see Docs/tasks/T-22.md).
/// T-490 (05 §5.9.12): `sheetDir` is the directory of the stylesheet file `text` was read from,
/// stamped on every StyleRule and FontFaceDef so their asset references resolve against it first;
/// empty for an inline `<style>` or a sheet that has no directory.
bool parseXts(std::string_view text, std::string_view fileName, int lineOffset, DiagnosticSink& sink,
              RuleSet& out, std::string_view sheetDir = {});

/// Parses a bare declaration list, e.g. `style="width:10px; color:#000"` or T-35's `setStyle`
/// argument. Same error recovery as parseXts's rule body. Returns false on error. T-335: a
/// `linear()` easing appends its points to `easingPoints` (rejected when null).
bool parseDeclarations(std::string_view text, std::string_view fileName, int line, DiagnosticSink& sink,
                       std::vector<StyleDecl>& out, std::vector<EasingPoint>* easingPoints = nullptr);

/// Parses a single property value with no shorthand expansion and no diagnostics (used by T-35's
/// runtime mini-parser, which will move this function's core to xt_core). `prop` must already be
/// resolved to a non-shorthand PropId/StyleProp number. Returns false if the text doesn't parse as
/// that property's value type.
bool parseValue(uint16_t prop, std::string_view text, StyleValue& out);

/// T-173 (05.3/05.7): a selector's packed `(id, class+pseudo+attribute, tag)` specificity, with
/// each `:not()` counting as its most specific argument. Used by the parser and by the symbol
/// expansion that rewrites selectors afterwards.
uint32_t selectorSpecificity(const Selector& sel);

}  // namespace xt::compile
