#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xt/gpu/gpu_device.h"
#include "xt/node.h"

/// Gradient stop lookup tables (Docs/tasks/T-146.md "漸層 stops 以 Texture 形式"): one
/// `kGradientLutWidth x 1` RGBA8 straight-color texture per distinct stop list, keyed by a hash
/// of the stops' bytes and shared by every command of the frame with the same stops. Entries
/// unused for a whole frame are destroyed at the end of that frame (`endFrame`), so an animated
/// gradient re-uploads 1 KB per frame and a static one uploads once.

namespace xt::gpu::detail {

class GradientLutCache {
public:
    void init(GpuDevice* device) { m_device = device; }
    /// Destroys every table.
    void shutdown();

    /// Returns the table for `stops`, creating and uploading it on a miss. 0 when `stops` is
    /// empty or the device refused the texture.
    TextureHandle get(std::span<const GradientStop> stops);

    /// Destroys the tables `get` did not touch since the previous `endFrame`.
    void endFrame();

private:
    struct Entry {
        uint64_t hash = 0;
        TextureHandle texture = 0;
        bool used = false;
    };
    GpuDevice* m_device = nullptr;
    std::vector<Entry> m_entries;
    std::vector<uint8_t> m_scratch;
};

/// Evaluates the stop ramp at `count` evenly spaced t values in [0, 1] (t = i / (count - 1)) into
/// `out` (RGBA8 straight color, 4 bytes per sample), matching Code/Runtime/gl/src/gl_shaders.h's
/// `sampleGradient`: t at or before the first stop takes the first color, at or after the last
/// stop the last color, and lerps linearly in between. Empty `stops` yield transparent black.
void bakeGradientLut(std::span<const GradientStop> stops, uint32_t count, std::vector<uint8_t>& out);

}  // namespace xt::gpu::detail
