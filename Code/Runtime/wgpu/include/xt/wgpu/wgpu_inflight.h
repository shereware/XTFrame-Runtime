#pragma once

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cstdint>
#include <string>

/// WgpuFramesInFlight (T-674, Docs/design/50-c-abi.md §50.ac item 1): at most kMaxFrames frames of one
/// `WGPUDevice` in flight. wgpu-core's device-level command encoder pool never shrinks and grows to the
/// most encoders in flight at once, so a CPU that runs ahead of the GPU without bound (an offscreen
/// target) grows it on every frame the GPU falls behind.
///
///   - `noteSubmit` takes each `wgpuQueueSubmitForIndex` index of the current frame (the largest is the
///     frame's last submission; every submit on one queue gets a larger index than the one before).
///   - `endFrame` keeps the frame's last index (the last one known when the frame has no submit of its
///     own) in a ring of the last kMaxFrames frames, and asks the queue to report when the work
///     submitted so far is done (`wgpuQueueOnSubmittedWorkDone`; the report arrives in a later
///     `wgpuDevicePoll`).
///   - `beginFrame` of frame N waits, with `wgpuDevicePoll(device, true, &index)`, for frame N - 2's
///     index -- only when that frame is not reported done after a non-blocking poll, so a GPU that is
///     not more than two frames behind is never waited for. Such a wait is counted (`waits`,
///     `waitMs`, and `frameWaits` / `frameWaitMs` for the frame it began). A frame still not reported
///     done after the wait (wgpu.h's `wgpuDevicePoll` returns no error; wgpu-native reports its own)
///     raises one warning (`takeWarning`) per object; the frame goes on either way.
///   - `waitIdle` waits for the last index noted (before resources are released); not counted.
///
/// The limit is the compile-time constant kMaxFrames; it is not configurable. Not thread-safe: use one
/// object from the thread that submits and polls its device. Used by the C ABI's GPU target
/// (capi_gpu.cpp) and by `WgpuRenderer` for a host's own frame loop (Code/Backends/xt_renderer_wgpu.cpp).

namespace xt::wgpu {

class WgpuFramesInFlight {
public:
    static constexpr uint32_t kMaxFrames = 2;

    WgpuFramesInFlight();
    ~WgpuFramesInFlight();
    WgpuFramesInFlight(const WgpuFramesInFlight&) = delete;
    WgpuFramesInFlight& operator=(const WgpuFramesInFlight&) = delete;

    /// A submission of the current frame (or between frames: it then belongs to the next frame ended).
    void noteSubmit(WGPUSubmissionIndex index);
    /// Starts a frame: ends one still open (with `queue`), then waits for the frame kMaxFrames before
    /// this one when the GPU has not finished it.
    void beginFrame(WGPUDevice device, WGPUQueue queue);
    /// Ends the open frame (nothing when none is open).
    void endFrame(WGPUQueue queue);
    /// Waits until the last submission noted is done (nothing when none was noted).
    void waitIdle(WGPUDevice device);
    /// Forgets the frames and indices (the device is released; call `waitIdle` first). The counters stay.
    void reset();

    /// The waits of the last `beginFrame` (0 or 1) and their milliseconds.
    uint32_t frameWaits() const { return m_frameWaits; }
    float frameWaitMs() const { return m_frameWaitMs; }
    /// Since construction: the waits that happened and their milliseconds.
    uint64_t waits() const { return m_waits; }
    double waitMs() const { return m_waitMs; }
    /// The most frames found not yet done at a `beginFrame` (after its non-blocking poll, before its
    /// wait); kMaxFrames at most while the waits work.
    uint32_t peakInFlight() const { return m_peakInFlight; }
    /// The one-time warning raised since the last call (a wait that returned before its frame was
    /// reported done), then cleared.
    std::string takeWarning();

private:
    /// What the work-done callbacks write; outlives this object while a callback is pending.
    struct Reports;
    struct Slot {
        uint64_t frame = 0;  ///< frame serial (0 = empty)
        WGPUSubmissionIndex index = 0;
        bool valid = false;  ///< a submission was noted by the end of that frame
    };

    static void onWorkDone(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1,
                           void* userdata2);
    uint64_t completed() const;
    void releaseReports();

    Reports* m_reports = nullptr;
    Slot m_ring[kMaxFrames];
    uint64_t m_serial = 0;  ///< the last frame begun (frames count from 1)
    bool m_open = false;
    WGPUSubmissionIndex m_lastIndex = 0;
    bool m_hasIndex = false;
    uint32_t m_frameWaits = 0;
    float m_frameWaitMs = 0;
    uint64_t m_waits = 0;
    double m_waitMs = 0;
    uint32_t m_peakInFlight = 0;
    bool m_warned = false;
    std::string m_warning;
};

}  // namespace xt::wgpu
