#pragma once

#include <webgpu/webgpu.h>

#include <string>

/// Small helpers shared by the xt_wgpu sources (T-437).

namespace xt::wgpu {

/// A WGPUStringView as a std::string ({NULL, WGPU_STRLEN} and {NULL, 0} are empty).
inline std::string toString(WGPUStringView s) {
    if (s.data == nullptr) {
        return std::string();
    }
    return s.length == WGPU_STRLEN ? std::string(s.data) : std::string(s.data, s.length);
}

/// The uncaptured errors and device losses reported by the devices `WgpuInit::headless` created
/// since the last call (wgpu_init.cpp), then cleared. One process-wide sink.
std::string takeDeviceErrors();

}  // namespace xt::wgpu
