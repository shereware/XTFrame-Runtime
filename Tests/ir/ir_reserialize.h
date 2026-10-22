#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xt/ir.h"

/// T-267c (Docs/design/15 §15.2): test-only IR re-serializer. Reads every chunk of an open
/// `IrView` through its public readers only and writes it back through `IrWriter`'s public
/// interface, so a reader that reads a field from the wrong offset shows up as a byte difference
/// against the compiled blob. Never linked into a product library.

namespace xt::test {

/// One chunk kind that cannot be rebuilt from the public readers and is copied verbatim instead.
struct CopiedChunk {
    /// The chunk's fourcc, e.g. "SHDR".
    const char* tag;
    /// What the public read / write interface lacks.
    const char* missing;
};

/// The chunk kinds `reserialize()` copies verbatim (the table at the top of ir_reserialize.cpp).
std::span<const CopiedChunk> copiedChunks();

/// Re-serializes `view` (see the file comment). Chunks are emitted in `IrWriter::finish()`'s
/// natural order.
std::vector<uint8_t> reserialize(const IrView& view);

/// The fourcc of the chunk that contains byte `offset` of the well-formed blob `bytes`; "header"
/// for the file header, "?" past the last chunk.
std::string chunkTagAt(std::span<const uint8_t> bytes, size_t offset);

}  // namespace xt::test
