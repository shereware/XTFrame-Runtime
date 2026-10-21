#pragma once

#include <string>
#include <string_view>

#include "xt/compile/diagnostics.h"
#include "xt/compile/dom.h"

/// Hand-written recursive-descent parser for the `.xt` XML-style document format into a DomTree.
/// See docs/design/05-style-and-css.md §5.1-5.2, docs/tasks/T-21.md.

namespace xt::compile {

/// The deepest element nesting a `.xt` document may have (the `<xt>` root is depth 1). A deeper
/// document gets one "maximum nesting depth" error and the parse stops there (T-257); the
/// editor's source scanner uses the same limit.
inline constexpr int kMaxXtDepth = 256;

/// T-406 (docs/design/43-compatibility-and-versions.md §43.2): the `.xt` format version, the root's
/// `<xt version="1">`. A missing `version` reads as 1; a value that is not a positive integer, or
/// one newer than this, is an error ("... a newer XTFrame is needed"). The attribute never reaches
/// the IR.
inline constexpr int kXtFormatVersion = 1;

/// Parses `text` (the contents of a `.xt` file named `fileName`, used only for diagnostics) into
/// `out`. Returns false and leaves `out` in an unspecified state if any error diagnostic is
/// produced; warnings do not affect the return value.
bool parseXt(std::string_view text, std::string_view fileName, DiagnosticSink& sink, DomTree& out);

/// Same as `parseXt`, for a symbol library (`<link rel="symbols" href>`, docs/design/26-layers-
/// symbols-library.md §26.2.1, docs/tasks/T-171.md): the `<xt>` root is still required, but a
/// `<body>` is not; the library's `<symbol>` definitions end up in `out.symbolDefs`.
bool parseXtSymbolLibrary(std::string_view text, std::string_view fileName, DiagnosticSink& sink,
                          DomTree& out);

}  // namespace xt::compile
