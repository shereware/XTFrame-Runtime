#pragma once

#include "binding_runtime.h"
#include "xt/document.h"

/// T-723 (Docs/design/10-backends.md §10.16 W5): the test-only switch between the narrow style
/// invalidation (the default) and the old full-range one -- every style binding write and state change
/// marks the element, every ancestor and its whole subtree kDirtyStyle, as before T-723. Only the
/// equivalence check (Tests/golden/golden_style_invalidation.cpp) flips it, to compare the
/// computed styles of both modes after the same interaction. Not part of any public API: this
/// header lives in Code/Runtime/core/src and the flag belongs to the Document's BindingRuntime.

namespace xt {

struct StyleInvalidationTestAccess {
    /// Selects the old full-range invalidation (`on`) or the narrow one (`!on`, the default).
    static void setLegacy(Document& doc, bool on) { BindingRuntime::setLegacyStyleInvalidation(doc, on); }
    /// True while `doc` uses the old full-range invalidation.
    static bool legacy(const Document& doc) { return BindingRuntime::legacyStyleInvalidation(doc); }
};

}  // namespace xt
