#pragma once

#include <map>
#include <string>
#include <string_view>
#include <unordered_map>

#include "style_emit.h"  // TokenScope (T-170).
#include "xt/compile/compile.h"
#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"
#include "xt/compile/ir_writer.h"
#include "xt/types.h"

/// `DomTree` -> `DocBuilder` (`DOCT`, Docs/design/02-ir-format.md §2.9), plus `img src` -> `ASST`
/// Image assets. See Docs/design/12-compiler.md §12.2 step 6, Docs/tasks/T-23.md.

namespace xt::compile {

/// `dom.nodes` is already exactly the DOCT element order (parse_xt.cpp appends each DomNode in
/// depth-first preorder, root first, matching Docs/design/02-ir-format.md §2.9's `DocElement`
/// order), so `doc.elements[i]` corresponds 1:1 to `dom.nodes[i]`.
/// `sceneBySrc` is compile_document's step-3 result (`<anim src>` -> SceneId, keyed by the literal
/// `src` attribute text); a `src` missing from the map means step 3 already reported an error for
/// it, so the element's `scene` is left `kInvalidScene`. `keyframesByName`/`style` are threaded
/// through to encodeDeclValue (style_emit.h) for `style="..."` inline declarations, which share
/// STYL's transitions/animations/fontFamilies pools. `xtDir` is the `.xt` file's own directory
/// (relative `img src` resolves against it, per T-23's ".xt 所在目錄" rule).
/// `tokens` (T-170) resolves `var()` in `style="..."` declarations; null means no tokens.
/// `l10n` (T-179, 07.8) receives the localization keys: a text node, or a `placeholder`/`alt`/
/// `title` attribute value, that starts with `@` is the key `@<name>` (the text/value written to
/// DOCT is `<name>`, which is also what shows until a provider resolves it); `\@` at the start
/// is a literal `@`. `l10n->keys`/`textKeys`/`attrKeys` are filled (the last two stay empty when
/// the document has no key); null skips key detection entirely.
/// `rules` (T-178) are the document's style rules, used to give rich-text runs with a `class` the
/// declarations of the plain `.class` rules they match (07.8); null means none.
/// Returns false if any error diagnostic was raised.
bool emitDoc(const DomTree& dom, IrWriter& writer, StyleBuilder& style, DocBuilder& doc,
             const std::map<std::string, SceneId>& sceneBySrc,
             const std::unordered_map<std::string, TimelineId>& keyframesByName, std::string_view xtDir,
             std::string_view fileName, const CompileOptions& options, DiagnosticSink& sink,
             const TokenScope* tokens = nullptr, LocalizationBuilder* l10n = nullptr,
             const std::vector<StyleRule>* rules = nullptr);

/// T-179: how a text node or localizable attribute value reads (07.8's `@key` syntax).
enum class LocalizedText : uint8_t {
    Literal,  ///< Plain text; `out` is the text with a leading `\@` unescaped to `@`.
    Key,      ///< `@<key>`; `out` is the key without its `@`.
    Invalid,  ///< Starts with `@` but the rest is not a valid key.
};

/// Classifies `text`. A key follows the id rule: an ASCII letter or `_`, then ASCII letters,
/// digits, `_` or `-`.
LocalizedText parseLocalizedText(std::string_view text, std::string& out);

}  // namespace xt::compile
