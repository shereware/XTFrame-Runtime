#pragma once

#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cstdint>
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

/// T-670 (Docs/design/50-c-abi.md §50.ab addendum 1 ii): the `WGPUNativeLimits.maxNonSamplerBindings`
/// `WgpuInit::headless` requires (wgpu's default is 1,000,000; the D3D12 shader-visible descriptor
/// heap has that many entries). `WgpuDevice` warns once when its live bindings pass 75% of it.
constexpr uint32_t kMaxNonSamplerBindings = 131072;
constexpr uint32_t kBindingsWarnThreshold = kMaxNonSamplerBindings / 4 * 3;

/// T-670 (50 §50.ab addendum 1 i): the instance backend masks `WgpuInit::headless` tries.
struct InstancePlan {
    WGPUInstanceBackend first = 0;  ///< the mask of the first instance
    bool retryPrimary = false;      ///< no adapter / device on it -> a second instance with Primary
};

/// `backendName` (WgpuHeadlessOptions::backend, else XT_WGPU_BACKEND) -> the plan: empty on Windows
/// = DX12 only, then Primary; empty elsewhere = Primary; vulkan|d3d12|metal|gl = that backend alone,
/// no retry. False for any other name. `windows` is the platform (a parameter for the unit test).
bool planInstance(const std::string& backendName, bool windows, InstancePlan& plan);

}  // namespace xt::wgpu
