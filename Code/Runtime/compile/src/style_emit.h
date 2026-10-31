#pragma once

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/compile/style_model.h"
#include "xt/compile/xtfx.h"  // XtfxParam (T-203).
#include "xt/types.h"
#include "xt/value.h"

/// `RuleSet` -> `StyleBuilder` (`STYL`), plus the `@keyframes` -> `TimelineBuilder` pass and the
/// shared `StyleValue` -> `IrDecl`/`IrKeyframe` 16-byte value encoder (also used by doc_emit.cpp
/// for `style="..."` inline declarations, which share STYL's transitions/animations/fontFamilies
/// pools). See Docs/design/02-ir-format.md §2.6, §2.9, Docs/design/12-compiler.md §12.3,
/// Docs/tasks/T-23.md.

namespace xt::compile {

/// T-170 (Docs/design/05-style-and-css.md §5.9.2-5.9.3, 12-compiler.md §12.7): the document's
/// design tokens, name (with `--`) -> value text with every `var()` inside it already substituted.
struct TokenScope {
    std::map<std::string, std::string> text;
};

/// T-170 step 2a: collects `ruleSet.tokens` into `out` (a repeated name warns and the later one
/// wins; a token referring to an undefined token or to itself warns and is dropped) and writes
/// each one into `writer`'s `VARS` table, typed as Color, Float or Length when its text is one,
/// else kept as text (StringRef).
void emitTokens(const RuleSet& ruleSet, IrWriter& writer, DiagnosticSink& sink, TokenScope& out);

/// T-170: rewrites `decls` against `tokens`. A VarSubst declaration is substituted and re-parsed
/// (shorthands expand as usual); an undefined token without a fallback warns and yields the
/// property's initial value (a shorthand's declaration is dropped instead). A VarRef declaration
/// is kept when `keepRefs` -- with a fallback always present (the initial value when none was
/// written; an undefined token warns, a token whose value does not fit the property warns) --
/// and otherwise (`@keyframes`, which has no cascade to resolve it in) replaced by its value.
void resolveVarDecls(std::vector<StyleDecl>& decls, const TokenScope& tokens, bool keepRefs,
                     std::string_view fileName, DiagnosticSink& sink,
                     const std::string& note = std::string());

/// T-170: resolveVarDecls() over every rule (keepRefs) and every `@keyframes` stop (flattened).
void resolveRuleSetVars(RuleSet& ruleSet, const TokenScope& tokens, std::string_view fileName,
                        DiagnosticSink& sink);

/// Step 4 of Docs/tasks/T-23.md's pipeline: one TimelineBuilder per `@keyframes` block (added to
/// `writer`), one IrKeyframesRef per block (appended to `style.keyframesRefs`), and the resulting
/// name -> TimelineId map (used by step 5 to resolve `animation-name`).
void emitKeyframesTimelines(const RuleSet& ruleSet, IrWriter& writer, StyleBuilder& style,
                            std::string_view fileName, DiagnosticSink& sink,
                            std::unordered_map<std::string, TimelineId>& outKeyframesByName);

/// Step 5: expands each rule's grouped selector list into one IrRule per Selector (source order =
/// expanded index), encodes every declaration, and turns `@font-face` into an ASST Font asset plus
/// a `style.fontFaces[]` entry. `xtDir` is the `.xt` file's own directory (relative `@font-face`
/// `src` resolves against it, per T-23's ".xt 所在目錄" rule -- NOT `options.rootDir`, which
/// `compileDocument` does not use; see its report). Returns false if any error diagnostic was
/// raised (some rules/decls/font-faces may still have been partially emitted either way; the
/// caller only inspects `sink.hasErrors()`/`strict`, matching Docs/tasks/T-23.md "任何 error -> ir
/// 空").
bool emitStyle(const RuleSet& ruleSet, IrWriter& writer, StyleBuilder& style,
               const std::unordered_map<std::string, TimelineId>& keyframesByName, std::string_view xtDir,
               std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink);

/// Docs/design/28-animation-editing.md §28.1 / 12-compiler.md §12.7 (T-172): every `@timeline` in
/// `ruleSet` becomes one TimelineGroup appended to `out` (the `TGRP` chunk). Each `#id` track (all
/// tracks naming the same element merged) becomes an internal `@keyframes <timeline>__<path>`
/// (added through emitKeyframesTimelines(), so it also lands in `keyframesByName` and
/// `style.keyframesRefs`), normalized over the track's own first..last key, plus a group member
/// `(element, timeline, delay = first key, duration = last - first)`. `effect.<name|index>.<param>`
/// declarations resolve against the element's state-less `effect` sequence (18.12). Errors
/// (unknown id, duplicate element/property/time, duplicate timeline name, unresolvable effect
/// address) are reported into `sink` and drop the whole timeline.
void emitTimelineGroups(const RuleSet& ruleSet, const DomTree& dom, IrWriter& writer, StyleBuilder& style,
                        std::unordered_map<std::string, TimelineId>& keyframesByName, DiagnosticSink& sink,
                        TimelineGroupBuilder& out);

/// T-544 (18 §18.12): resolves the effect address `<effectName>.<param>` (`effectName` an effect
/// name, a custom effect's header name, or a stack index written as digits) against the effect
/// sequence `seq` (may be null) into the 18.2 effect PropId `out`; `isColor` is true for a color
/// parameter. On failure `error` holds the message (no prefix) and false is returned. Shared by
/// `@timeline` (emitTimelineGroups) and `bind:effect.<effect>.<param>` (bind_emit.cpp).
bool resolveEffectAddress(const IrWriter& writer, std::string_view effectName, std::string_view param,
                          const std::vector<EffectDeclEntry>* seq, std::string& error, PropId& out,
                          bool& isColor);

/// T-544: the element's state-less `effect` sequence as far as the compiler can tell (the winning
/// `effect` declaration among the rules whose selectors carry no pseudo-class, then the inline
/// `style` attribute); nullptr when none. `scratch` backs the parsed inline declarations and must
/// outlive the returned pointer.
const std::vector<EffectDeclEntry>* staticEffectSequence(const RuleSet& ruleSet, const DomTree& dom,
                                                         uint32_t element, std::string_view fileName,
                                                         std::vector<StyleDecl>& scratch);

/// T-174: true when `sel` matches `element` in the state-less (base) cascade the compiler can
/// know: any compound carrying a pseudo-class or `:state()` never matches. Used for `.xts`
/// trigger declarations (trigger_emit.cpp), like the static `effect` lookup of 18.12.
bool selectorMatchesStatically(const Selector& sel, const DomTree& dom, uint32_t element);

/// Encodes one `StyleValue` into a Decl/Keyframe-format 16-byte value blob (Docs/design/02-ir-format.md
/// §2.6's `Decl.value[16]` encoding table). TransitionList/AnimationList/FontFamilyList append to
/// `style`'s corresponding pool; AnimationList additionally resolves each `animation-name` through
/// `keyframesByName` (an unresolved name reports `sink.error` with message `unknown @keyframes
/// "<name>"` and returns false). Neutral/Shorthand are never legal here (Neutral only exists inside
/// compiler-synthesized keyframes -- see emitKeyframesTimelines; Shorthand is always expanded away
/// by T-22's parser) and report an error too. `outBytes` must point at a 16-byte buffer.
bool encodeDeclValue(IrWriter& writer, StyleBuilder& style,
                     const std::unordered_map<std::string, TimelineId>& keyframesByName,
                     const StyleValue& value, std::string_view fileName, int line, DiagnosticSink& sink,
                     uint8_t outBytes[16], ValueType& outType);

/// Encodes a full StyleDecl (prop + value + !important) into an IrDecl via encodeDeclValue().
/// Returns false (nothing written to `out`) on the same failures as encodeDeclValue.
bool encodeDecl(IrWriter& writer, StyleBuilder& style,
                const std::unordered_map<std::string, TimelineId>& keyframesByName, const StyleDecl& decl,
                std::string_view fileName, DiagnosticSink& sink, IrDecl& out);

/// T-273c (`CompileOptions::publishAssetNames`): the text a published build writes for the
/// reference `src` (written in a document in `xtDir`) of a file that is compiled in or packed
/// file by file -- a `<link href>`, an `<anim src>`, an `<img src="*.svg">`, a frame-folder
/// `<sequence src>`: `src` as written when not publishing or when it lies inside the project
/// (assetLeavesProject()), else its last path element.
std::string publishedReference(const std::string& src, std::string_view xtDir, const CompileOptions& options);

/// T-199 (Docs/design/22-bundle.md §22.3, 26 §26.3.4): the ASST Image entry for the image `src`
/// as written in the source (an `<img src>`, `background-image`, `border-image-source`), created
/// on first use and shared afterwards (one entry per distinct source name, looked up in
/// `writer.assets()`). Resolves the file per resolveAsset(), embeds it when
/// `options.embedImages`, runs `options.assetNameRewrite`, and gives an `engine://` name its
/// verbatim entry (T-150). A rewritten name of the form `atlas:<n>#<asset>` gets ASST kind 3
/// (AtlasImage). `line`/`col` locate the warnings ("not found", "failed to embed"). T-490 (05
/// §5.9.12): `sheetDir` is the directory of the `<link>`ed stylesheet that declared `src`
/// (StyleRule::sheetDir), tried first by resolveAsset(); empty for an element attribute, an inline
/// `<style>` or an in-memory sheet.
AssetId ensureImageAsset(IrWriter& writer, const std::string& src, std::string_view xtDir,
                         std::string_view fileName, int line, int col, const CompileOptions& options,
                         DiagnosticSink& sink, std::string_view sheetDir = {});

/// T-203 (Docs/design/18-effects.md §18.7): the ASST Shader entry (kind 2) and `SHDR` entry for
/// the `.xtfx` asset `src` of a `custom("<src>")` effect, created on first use (resolved like any
/// asset, compiled with compileXtfx(), `options.assetNameRewrite` applied) and shared afterwards.
/// kInvalidAsset after reporting an error (not found: at `fileName:line`; a header/body error:
/// at its own line and column in the `.xtfx` file); a failed asset is reported once.
AssetId ensureShaderAsset(IrWriter& writer, const std::string& src, std::string_view xtDir,
                          std::string_view fileName, int line, const CompileOptions& options,
                          DiagnosticSink& sink);

/// T-203: the loaded `.xtfx` asset named `src` as written, or kInvalidAsset.
AssetId findShaderAsset(const IrWriter& writer, std::string_view src);

/// T-203: `shader`'s header parameter table rebuilt from its SHDR entry (slots included).
std::vector<XtfxParam> xtfxParamsOf(const ShaderBuilder& shader);

/// T-203: loads the asset of every `custom()` entry in `decls`' effect lists and fills the entry's
/// `params`/`colors` (header defaults, then the `custom()` overrides, each checked against the
/// declared type) and `shaderAsset`. A declaration with a failing entry is reported and removed.
/// Returns false when something was reported.
bool resolveCustomEffects(std::vector<StyleDecl>& decls, IrWriter& writer, std::string_view xtDir,
                          std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink);

/// T-203: the same over every rule, `@keyframes` stop and `@timeline` key of `ruleSet`.
bool resolveCustomEffects(RuleSet& ruleSet, IrWriter& writer, std::string_view xtDir,
                          std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink);

}  // namespace xt::compile
