#pragma once

#include <string_view>
#include <vector>

#include "xt/compile/diagnostics.h"
#include "xt/compile/style_model.h"
#include "xt/types.h"

/// Public entry point onto the `.xts` effect-list parser (Docs/design/18-effects.md §18.3-§18.4,
/// Docs/tasks/T-63.md). The parser itself lives in Code/Runtime/compile/src/xts/xts_values.cpp (internal
/// linkage, tokenized-declaration signature); this header exposes a string-in wrapper for callers
/// outside `xt::compile` (the editor's `EffectStack::parse()`) that only have raw `effect:` value
/// text, not an already-tokenized declaration.

namespace xt::compile {

/// Parses `text` (an `effect:` value, e.g. `"shadow(0 4px 12px #00000040) outline(1px #fff inner)"`
/// or `"none"`) into `out` (appended to, not cleared first is *not* the contract -- `out` is
/// replaced). Tokenizes `text` with the `.xts` lexer, then runs the same per-function parsing
/// `.xts` stylesheet compilation uses. Diagnostics (`custom()`'s rejection, bad arguments, percent
/// on a length, more than 16 effects, an unterminated `(`, a stray token) are recorded on `sink`
/// with `file == "<effect>"`, `line == 1` (this entry point has no source file/line of its own).
/// Returns `Status::kOk` on success (`out` holds the parsed sequence, empty for `"none"`),
/// `Status::kInvalidArgument` when `text` fails to lex or parse (see `sink` for why; `out` is left
/// empty).
Status parseEffectList(std::string_view text, std::vector<EffectDeclEntry>& out, DiagnosticSink& sink);

}  // namespace xt::compile
