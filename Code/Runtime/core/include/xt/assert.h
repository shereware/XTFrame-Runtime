#pragma once

/// Debug-only assertion macro for logic invariants that must never fire in correct code.
/// 見 Docs/tasks/T-02.md, Docs/design/14-coding-standards.md §14.3

namespace xt::detail {

/// Prints "XT_ASSERT failed: <expr> at <file>:<line>\n" to stderr then aborts.
[[noreturn]] void assertFail(const char* expr, const char* file, int line);

}  // namespace xt::detail

#if defined(NDEBUG)
#define XT_ASSERT(cond) ((void)0)
#else
#define XT_ASSERT(cond) ((cond) ? (void)0 : ::xt::detail::assertFail(#cond, __FILE__, __LINE__))
#endif
