#include "xt/assert.h"

#include <cstdio>
#include <cstdlib>

namespace xt::detail {

[[noreturn]] void assertFail(const char* expr, const char* file, int line) {
    std::fprintf(stderr, "XT_ASSERT failed: %s at %s:%d\n", expr, file, line);
    std::abort();
}

}  // namespace xt::detail
