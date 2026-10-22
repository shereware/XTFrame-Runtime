#include "xt/perf.h"

/// PerfTracker's ring buffer and summary math. See Docs/design/21-perf-tracing.md §21.2,
/// Docs/tasks/T-76a.md.

namespace xt {

void PerfTracker::beginFrame(float timeSeconds) {
    if (!m_enabled) {
        return;
    }
    if (m_frames.empty()) {
        m_frames.resize(kHistoryCapacity);
        m_count = 0;
        m_newest = kHistoryCapacity - 1;  // The advance below wraps it to slot 0.
    }
    m_newest = (m_newest + 1) % kHistoryCapacity;
    if (m_count < kHistoryCapacity) {
        ++m_count;
    }
    FrameStats& slot = m_frames[m_newest];
    slot = FrameStats{};
    slot.frameIndex = m_nextFrameIndex++;
    slot.timeSeconds = timeSeconds;
}

void PerfTracker::endFrame() {
    // The frame's record is already in the ring (beginFrame put it there), so there is nothing to
    // commit here. Keeping the slot addressable until the next beginFrame is deliberate: it is what
    // lets a backend add renderMs/effectsMs/textMs after Context::render() has returned (§21.2's
    // "後端回報").
}

std::span<const FrameStats> PerfTracker::history() const {
    if (m_count == 0) {
        return std::span<const FrameStats>{};
    }
    m_ordered.clear();
    m_ordered.reserve(m_count);
    uint32_t oldest = (m_newest + kHistoryCapacity - (m_count - 1)) % kHistoryCapacity;
    for (uint32_t i = 0; i < m_count; ++i) {
        m_ordered.push_back(m_frames[(oldest + i) % kHistoryCapacity]);
    }
    return std::span<const FrameStats>(m_ordered.data(), m_ordered.size());
}

PerfSummary PerfTracker::summary(uint32_t lastN) const {
    PerfSummary out;
    std::span<const FrameStats> all = history();
    if (all.empty() || lastN == 0) {
        return out;
    }
    uint32_t n = lastN < all.size() ? lastN : static_cast<uint32_t>(all.size());
    std::span<const FrameStats> window = all.subspan(all.size() - n, n);

    // Sums are kept in double/uint64 so a long window cannot lose precision or wrap.
    struct Sums {
#define XT_PERF_DECL_FLOAT(name) double name = 0;
        XT_FRAME_STATS_FLOAT_FIELDS(XT_PERF_DECL_FLOAT)
#undef XT_PERF_DECL_FLOAT
#define XT_PERF_DECL_UINT(name) uint64_t name = 0;
        XT_FRAME_STATS_UINT_FIELDS(XT_PERF_DECL_UINT)
#undef XT_PERF_DECL_UINT
    } sums;

    for (const FrameStats& f : window) {
#define XT_PERF_ACC_FLOAT(name)               \
    sums.name += static_cast<double>(f.name); \
    if (f.name > out.max.name) {              \
        out.max.name = f.name;                \
    }
        XT_FRAME_STATS_FLOAT_FIELDS(XT_PERF_ACC_FLOAT)
#undef XT_PERF_ACC_FLOAT
#define XT_PERF_ACC_UINT(name)   \
    sums.name += f.name;         \
    if (f.name > out.max.name) { \
        out.max.name = f.name;   \
    }
        XT_FRAME_STATS_UINT_FIELDS(XT_PERF_ACC_UINT)
#undef XT_PERF_ACC_UINT
    }

    out.frames = n;
    double inv = 1.0 / static_cast<double>(n);
#define XT_PERF_AVG_FLOAT(name) out.avg.name = static_cast<float>(sums.name * inv);
    XT_FRAME_STATS_FLOAT_FIELDS(XT_PERF_AVG_FLOAT)
#undef XT_PERF_AVG_FLOAT
#define XT_PERF_AVG_UINT(name) \
    out.avg.name = static_cast<uint32_t>(static_cast<double>(sums.name) * inv + 0.5);
    XT_FRAME_STATS_UINT_FIELDS(XT_PERF_AVG_UINT)
#undef XT_PERF_AVG_UINT
    out.avg.frameIndex = window.back().frameIndex;
    out.max.frameIndex = window.back().frameIndex;

    // §21.1: timeSeconds is the Context clock, so this is the frame rate of the clock the host
    // advances -- real elapsed time for a windowed host, fixed-step time for a headless run.
    float span = window.back().timeSeconds - window.front().timeSeconds;
    if (n > 1 && span > 0.0f) {
        out.fpsAvg = static_cast<float>(n - 1) / span;
    }
    return out;
}

void PerfTracker::reset() {
    m_frames.clear();
    m_ordered.clear();
    m_count = 0;
    m_newest = 0;
    m_nextFrameIndex = 0;
    m_sink = FrameStats{};
}

}  // namespace xt
