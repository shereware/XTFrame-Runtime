#include <cstdlib>
#include <new>

#include "xt/perf.h"

/// T-475 (Docs/design/21-perf-tracing.md §21.6, 27-interaction.md §27.4.2 item 6): the global
/// operator new replacement that feeds xt::perf::AllocCounter. Compiled only into xt_perf_flow,
/// with XT_PERF_ALLOC_COUNT defined; product libraries never replace the allocator, so their
/// FrameStats::flowAllocs stays 0.

#ifdef XT_PERF_ALLOC_COUNT

void* operator new(std::size_t size) {
    xt::perf::AllocCounter::record();
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new[](std::size_t size) {
    xt::perf::AllocCounter::record();
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    xt::perf::AllocCounter::record();
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    xt::perf::AllocCounter::record();
    return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

#endif
