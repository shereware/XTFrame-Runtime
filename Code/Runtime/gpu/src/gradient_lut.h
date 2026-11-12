#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/gpu/gpu_device.h"
#include "xt/node.h"

/// Gradient stop lookup tables (Docs/tasks/T-146.md "漸層 stops 以 Texture 形式"): one
/// `kGradientLutWidth x 1` RGBA8 straight-color texture per distinct stop list, keyed by a hash
/// of the stops' bytes and shared by every command of the frame with the same stops. T-695
/// (Docs/design/50-c-abi.md §50.ac note 3 item 1): a table is destroyed only after more than
/// `kGradientLutKeepFrames` consecutive frames of the builder without use (`endFrame`), and at
/// most `kGradientLutMaxTables` are kept -- a miss beyond that replaces the least recently used
/// table that the frame in progress has not drawn with. `shutdown` (renderer reset, device
/// change, xt_surface_trim) destroys them all; trim-on-hide keeps them.

namespace xt::gpu::detail {

/// T-695: frames a table may go unused and stay (1800 = 30 s at 60 fps).
inline constexpr uint64_t kGradientLutKeepFrames = 1800;
/// T-695: tables kept per cache (256 x 1 KiB).
inline constexpr size_t kGradientLutMaxTables = 256;

class GradientLutCache {
public:
    void init(GpuDevice* device) { m_device = device; }
    /// Destroys every table.
    void shutdown();

    /// Starts frame `frame` of the builder (its frame count); `get` stamps the tables it returns
    /// with it.
    void beginFrame(uint64_t frame) { m_frame = frame; }

    /// Returns the table for `stops`, creating and uploading it on a miss. 0 when `stops` is
    /// empty or the device refused the texture.
    TextureHandle get(std::span<const GradientStop> stops);

    /// Destroys the tables unused for more than `kGradientLutKeepFrames` frames, then the least
    /// recently used ones over `kGradientLutMaxTables` (never one the current frame used).
    void endFrame();

private:
    struct Entry {
        uint64_t hash = 0;
        TextureHandle texture = 0;
        uint64_t lastUsed = 0;  ///< the builder frame that last drew with it
    };
    /// Index of the least recently used entry the current frame has not used; size() when none.
    size_t leastRecentlyUnused() const;
    GpuDevice* m_device = nullptr;
    uint64_t m_frame = 0;
    std::vector<Entry> m_entries;
    std::vector<uint8_t> m_scratch;
};

/// Evaluates the stop ramp at `count` evenly spaced t values in [0, 1] (t = i / (count - 1)) into
/// `out` (RGBA8 straight color, 4 bytes per sample), matching Code/Runtime/gl/src/gl_shaders.h's
/// `sampleGradient`: t at or before the first stop takes the first color, at or after the last
/// stop the last color, and lerps linearly in between. Empty `stops` yield transparent black.
void bakeGradientLut(std::span<const GradientStop> stops, uint32_t count, std::vector<uint8_t>& out);

}  // namespace xt::gpu::detail
