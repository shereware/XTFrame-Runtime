#pragma once

#include <cstddef>

namespace xt::fuzz {

/// Feeds a value a fuzz target derived from a parse result into a volatile sink, so the reads that
/// produced it are neither dead code nor flagged as an unused variable.
inline void consume(size_t value) {
    static volatile size_t sink = 0;
    sink = sink + value;
}

}  // namespace xt::fuzz
