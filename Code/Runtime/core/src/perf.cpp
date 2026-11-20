#include "xt/perf.h"

#include <algorithm>
#include <cstring>

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
    // T-747: empty cause-node slots (0 is a valid NodeId).
    for (auto& row : slot.causeNodes) {
        for (uint32_t& id : row) {
            id = 0xFFFFFFFFu;  // kInvalidNode
        }
    }
    // T-750: the host's pending markFrameStart() belongs to this record.
    m_frameStarted = m_pendingStart;
    m_frameWall = m_pendingWall;
    m_frameCpu = m_pendingCpu;
    m_pendingStart = false;
}

void PerfTracker::markFrameStart() {
    if (!m_enabled) {
        return;
    }
    m_pendingStart = true;
    m_pendingCpu = m_threadCpu ? m_clock.nowMs() : 0.0;
    m_pendingWall = std::chrono::steady_clock::now();
}

void PerfTracker::markFrameEnd() {
    if (!m_enabled || !m_frameStarted || m_frames.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    FrameStats& f = m_frames[m_newest];
    f.frameWallMs = std::chrono::duration<float, std::milli>(now - m_frameWall).count();
    f.frameCpuMs = m_threadCpu ? static_cast<float>(m_clock.nowMs() - m_frameCpu) : 0.0f;
    f.frameMarked = 1;
    m_frameStarted = false;
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
        uint64_t causes[kDirtyCauseCount][kDirtyKindCount] = {};  // T-747
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
        // T-747: the cause table, like the other counts.
        for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {
            for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
                sums.causes[c][k] += f.causes[c][k];
                out.max.causes[c][k] = std::max(out.max.causes[c][k], f.causes[c][k]);
            }
        }
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
    for (uint32_t c = 0; c < kDirtyCauseCount; ++c) {  // T-747
        for (uint32_t k = 0; k < kDirtyKindCount; ++k) {
            out.avg.causes[c][k] = static_cast<uint32_t>(static_cast<double>(sums.causes[c][k]) * inv + 0.5);
        }
    }
    // The newest frame's cause nodes (an average of ids means nothing).
    std::memcpy(out.avg.causeNodes, window.back().causeNodes, sizeof(out.avg.causeNodes));
    std::memcpy(out.max.causeNodes, window.back().causeNodes, sizeof(out.max.causeNodes));
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
    m_pendingStart = false;  // T-750
    m_frameStarted = false;
    m_zones = PerfZoneTotals{};  // T-753
    m_zoneDepth = 0;
}

namespace {

/// T-753: PerfZone's names, CSV names and phases, in enumeration order.
struct ZoneInfo {
    const char* name;
    const char* csv;
    const char* phase;
};
constexpr ZoneInfo kZoneInfo[kPerfZoneCount] = {
    {"paint.text", "paint_text", "paint"},
    {"paint.paths", "paint_paths", "paint"},
    {"paint.borders", "paint_borders", "paint"},
    {"paint.effects", "paint_effects", "paint"},
    {"paint.images", "paint_images", "paint"},
    {"update.style.match", "update_style_match", "update"},
    {"update.style.cascade", "update_style_cascade", "update"},
    {"update.layout.flex", "update_layout_flex", "update"},
    {"update.layout.grid", "update_layout_grid", "update"},
    {"update.layout.textShape", "update_layout_text_shape", "update"},
    {"build.walk", "build_walk", "build"},
    {"build.effects", "build_effects", "build"},
    {"build.cache", "build_cache", "build"},
};

const ZoneInfo& zoneInfo(PerfZone zone) {
    const uint32_t z = static_cast<uint32_t>(zone);
    return kZoneInfo[z < kPerfZoneCount ? z : 0];
}

}  // namespace

const char* perfZoneName(PerfZone zone) { return zoneInfo(zone).name; }

const char* perfZoneCsvName(PerfZone zone) { return zoneInfo(zone).csv; }

const char* perfZonePhase(PerfZone zone) { return zoneInfo(zone).phase; }

}  // namespace xt

namespace xt::perf {

namespace {
// T-475: AllocCounter's count; per thread, so a tick only sees its own thread's allocations.
thread_local uint64_t t_allocations = 0;
}  // namespace

void AllocCounter::record() noexcept { ++t_allocations; }

uint64_t AllocCounter::total() noexcept { return t_allocations; }

}  // namespace xt::perf
