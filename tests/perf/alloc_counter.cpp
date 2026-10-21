#include "alloc_counter.h"

#include <cstdlib>
#include <new>

/// See alloc_counter.h. The global operator new/delete overrides at the bottom of this file are
/// what actually replaces the process-wide allocator; everything in namespace xt::perf is just
/// the thread_local bookkeeping they update.

namespace xt::perf {

namespace {

thread_local bool g_counting = false;
thread_local uint64_t g_count = 0;
thread_local uint32_t g_sizes[kMaxRecordedSizes] = {};
thread_local uint32_t g_sizeCount = 0;

/// Called from the global operator new/new[] overrides below. Never throws (they must not fail
/// because bookkeeping failed).
void recordAlloc(std::size_t size) noexcept {
    if (!g_counting) {
        return;
    }
    ++g_count;
    if (g_sizeCount < static_cast<uint32_t>(kMaxRecordedSizes)) {
        g_sizes[g_sizeCount++] = static_cast<uint32_t>(size);
    }
}

}  // namespace

void startCounting() {
    g_counting = false;  // Disabled while resetting, so the reset itself is never counted.
    g_count = 0;
    g_sizeCount = 0;
    g_counting = true;
}

void stopCounting() { g_counting = false; }

uint64_t count() { return g_count; }

uint32_t recordedSize(int index) {
    return (index >= 0 && index < static_cast<int>(g_sizeCount)) ? g_sizes[index] : 0;
}

uint32_t recordedCount() { return g_sizeCount; }

}  // namespace xt::perf

// ---------------------------------------------------------------------------------------------
// Global operator new/delete replacement (must be declared at global scope, not inside a
// namespace, to actually replace the language's default allocation functions).
// ---------------------------------------------------------------------------------------------

#if 1
void* operator new(std::size_t size) {
    xt::perf::recordAlloc(size);
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    xt::perf::recordAlloc(size);
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
