#pragma once

#include <cstdint>

/// Global operator new/delete replacement used by xt_perf's zero-allocation tests
/// (test_zero_alloc.cpp). Linked only into the standalone xt_perf executable: replacing
/// operator new/delete is process-wide, so it must never share a binary with xt_tests (see
/// Docs/tasks/T-38.md).

namespace xt::perf {

/// How many allocation sizes startCounting()/count() keeps for diagnostics.
inline constexpr int kMaxRecordedSizes = 32;

/// Resets the per-thread counters (count() and the recorded sizes) and starts counting operator
/// new / new[] calls. Context is single-threaded (Docs/design/01-architecture.md §1.6) and every
/// xt_perf TEST_CASE runs on doctest's single test thread, so a thread_local counter is enough --
/// this is not meant to be re-entrant across threads.
void startCounting();
/// Stops counting; count() and the recorded sizes keep their last values until the next
/// startCounting().
void stopCounting();

/// Number of operator new / new[] calls observed since the last startCounting().
uint64_t count();

/// The first recordedCount() allocation sizes observed since the last startCounting(), in order.
/// index must be in [0, recordedCount()).
uint32_t recordedSize(int index);
/// min(count(), kMaxRecordedSizes).
uint32_t recordedCount();

}  // namespace xt::perf
