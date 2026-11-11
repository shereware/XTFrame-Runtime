#include "xt/wgpu/wgpu_inflight.h"

#include <algorithm>
#include <chrono>

namespace xt::wgpu {

/// Written by the work-done callbacks (from inside `wgpuDevicePoll` / a submit on the owner's thread,
/// as wgpu-native runs them). `pending` counts callbacks not yet run; an owner that goes away while
/// some are pending leaves the block to the last of them (`orphaned`).
struct WgpuFramesInFlight::Reports {
    uint64_t completed = 0;  ///< the latest frame reported done
    uint32_t pending = 0;
    bool orphaned = false;
};

void WgpuFramesInFlight::onWorkDone(WGPUQueueWorkDoneStatus /*status*/, WGPUStringView /*message*/,
                                    void* userdata1, void* userdata2) {
    // Any status ends the wait for that frame: success, or a device / callback that will never report it.
    auto* reports = static_cast<Reports*>(userdata1);
    const auto frame = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(userdata2));
    reports->completed = std::max(reports->completed, frame);
    if (reports->pending > 0) --reports->pending;
    if (reports->orphaned && reports->pending == 0) delete reports;
}

WgpuFramesInFlight::WgpuFramesInFlight() : m_reports(new Reports) {}

WgpuFramesInFlight::~WgpuFramesInFlight() { releaseReports(); }

void WgpuFramesInFlight::releaseReports() {
    if (m_reports == nullptr) return;
    if (m_reports->pending > 0) {
        m_reports->orphaned = true;  // the last pending callback deletes it
    } else {
        delete m_reports;
    }
    m_reports = nullptr;
}

uint64_t WgpuFramesInFlight::completed() const { return m_reports->completed; }

void WgpuFramesInFlight::noteSubmit(WGPUSubmissionIndex index) {
    m_lastIndex = m_hasIndex ? std::max(m_lastIndex, index) : index;
    m_hasIndex = true;
}

void WgpuFramesInFlight::beginFrame(WGPUDevice device, WGPUQueue queue) {
    if (m_open) endFrame(queue);
    m_frameWaits = 0;
    m_frameWaitMs = 0;
    ++m_serial;
    m_open = true;
    if (device == nullptr || m_serial <= kMaxFrames) return;
    const uint64_t target = m_serial - kMaxFrames;
    const Slot& slot = m_ring[target % kMaxFrames];
    if (slot.frame != target || !slot.valid || completed() >= target) return;
    wgpuDevicePoll(device, 0, nullptr);  // runs the reports of the work already finished
    const uint64_t done = completed();
    m_peakInFlight =
        std::max(m_peakInFlight, static_cast<uint32_t>(m_serial - 1 - std::min(done, m_serial - 1)));
    if (done >= target) return;
    const auto t0 = std::chrono::steady_clock::now();
    wgpuDevicePoll(device, 1, &slot.index);  // WGPUBool: whether the queue is empty -- not an error
    const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m_frameWaits = 1;
    m_frameWaitMs = ms;
    ++m_waits;
    m_waitMs += ms;
    if (completed() < target && !m_warned) {
        m_warned = true;
        m_warning = "frame " + std::to_string(target) + " (submission " + std::to_string(slot.index) +
                    ") was not reported done after a blocking wgpuDevicePoll; frames go on";
    }
}

void WgpuFramesInFlight::endFrame(WGPUQueue queue) {
    if (!m_open) return;
    m_open = false;
    Slot& slot = m_ring[m_serial % kMaxFrames];
    slot.frame = m_serial;
    slot.valid = m_hasIndex;
    slot.index = m_lastIndex;
    if (!m_hasIndex || queue == nullptr) {
        m_reports->completed = std::max(m_reports->completed, m_serial);  // nothing to wait for
        return;
    }
    WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
    info.mode = WGPUCallbackMode_AllowSpontaneous;
    info.callback = &onWorkDone;
    info.userdata1 = m_reports;
    info.userdata2 = reinterpret_cast<void*>(static_cast<uintptr_t>(m_serial));
    ++m_reports->pending;
    wgpuQueueOnSubmittedWorkDone(queue, info);
}

void WgpuFramesInFlight::waitIdle(WGPUDevice device) {
    if (device == nullptr || !m_hasIndex) return;
    wgpuDevicePoll(device, 1, &m_lastIndex);
}

void WgpuFramesInFlight::reset() {
    releaseReports();
    m_reports = new Reports;
    for (Slot& s : m_ring) s = Slot{};
    m_serial = 0;
    m_open = false;
    m_lastIndex = 0;
    m_hasIndex = false;
    m_frameWaits = 0;
    m_frameWaitMs = 0;
}

std::string WgpuFramesInFlight::takeWarning() {
    std::string w;
    w.swap(m_warning);
    return w;
}

}  // namespace xt::wgpu
