#pragma once

#include <string_view>

/// T-587 (Docs/design/54-source-format-v2.md §54.5, 46-style-stack.md §46.14): which source
/// format v2 section a property belongs to. The table is generated from the style group data
/// (Tools/Scripts/gen_property_table.py --section-table, xts_section_table.inc).

namespace xt::compile {

/// True when `name` (a CSS property name as written, after alias resolution) may be written in a
/// v2 `<layout>` section: the `placement`, `autolayout`, `responsive` and `space3d` groups, `layer`
/// and the anchor shorthands `anchor` / `offset` / `offset-end` / `size`. Every other property
/// belongs to `<style>`.
bool isLayoutSectionProperty(std::string_view name);

/// T-588 (54 §54.5.1): true for the compile-time anchor shorthands of the layout group (`anchor`,
/// `offset`, `offset-end`, `size`), which only a v2 `<layout>` section accepts and which the parser
/// expands into longhand declarations (anchor_expand.h); they have no StyleProp number.
bool isLayoutShorthandProperty(std::string_view name);

}  // namespace xt::compile
