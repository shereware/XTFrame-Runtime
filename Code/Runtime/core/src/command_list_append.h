#pragma once

#include <cstdint>

#include "xt/command_list.h"

/// T-748 (Docs/design/23-host-integration.md §23.ag item 3): Context::render() reuses the commands a
/// Document painted into the previous frame's list instead of painting it again.

namespace xt {

/// Appends commands [`first`, `end`) of `src` to `out`, with every payload and every pool range they
/// refer to (path verbs and points, gradient stops, dashes, glyphs and their positions, effect
/// instances and their stops, the ShaderRefs of Custom effects with their text and parameters,
/// particle instances, mesh vertices and indices) copied into `out`'s pools and the offsets rewritten.
/// Each appended command hashes (hashCommandList) and draws exactly as its original. `src.sources`
/// is not read; the commands keep their `source` indices. `first` <= `end` <= src.commands.size().
void appendCommandRange(const CommandList& src, uint32_t first, uint32_t end, CommandList& out);

}  // namespace xt
