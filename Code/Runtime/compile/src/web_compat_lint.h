#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

/// T-594 (Docs/design/54-source-format-v2.md §54.14 S4, 24 §24.15.4): the `web.*` warnings of
/// CompileOptions::webCompat -- a habit from HTML/CSS that XTFrame does not share, each with a
/// `fix` and a pointer to its item in Skills/skills/xtui/references/not-like-web.md. Every check
/// reports through DiagnosticSink::web(), which records nothing unless the sink's webCompat() is on.

namespace xt::compile {

class DiagnosticSink;
struct DomTree;

/// One `prop: value` declaration of a stylesheet, a v2 section or a `style` attribute (the
/// parser's raw text, before validation): `web.inlineBlock`, `web.floatOrTable`,
/// `web.positionFixed`, `web.boxSizing`, `web.vwVhVsUiscale`.
void lintWebDeclaration(std::string_view prop, std::string_view value, const std::string& file, int line,
                        int col, DiagnosticSink& sink);

/// `web.textWithChildren`: the inline children of `nodeIndex` that are about to be folded into its
/// rich text (parse_xt's buildRichText()) and carry an `id`, `bind:` or `on:` attribute, which the
/// folding drops.
void lintWebRichText(const DomTree& tree, uint32_t nodeIndex, const std::string& file, DiagnosticSink& sink);

/// The parsed element tree: `web.floatOrTable` (`<table>` markup) and `web.layerAttribute` (a
/// `layer` attribute, which does nothing).
void lintWebDom(const DomTree& dom, const std::string& file, DiagnosticSink& sink);

/// The finished IR (its computed style, dumpComputedStyle()): `web.zIndexAcrossLayers`,
/// `web.pointerEventsInherit`, `web.flexTextNoMinWidth`, `web.percentHeightOnAuto`,
/// `web.translateZSort`. Reported at the element's SRCM line in `file` (the main `.xt`).
/// T-762 (19 §19.y.2): also, whether or not webCompat is on, a note (DiagnosticSink::note(),
/// `compile.note`) for a flat element on a perspective root's plane whose grouping property (opacity
/// < 1, a blend mode, an effect, an overflow clip, a clip-path) keeps its 3D descendants out of the
/// space's depth sort; run only when the document declares a `perspective`.
void lintWebDocument(std::span<const uint8_t> ir, const std::string& file, DiagnosticSink& sink);

}  // namespace xt::compile
