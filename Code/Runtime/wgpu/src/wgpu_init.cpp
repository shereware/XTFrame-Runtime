#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "wgpu_util.h"
#include "xt/wgpu/wgpu_renderer.h"

/// WgpuInit::headless (T-437, Docs/design/10-backends.md §10.10.1): instance -> adapter -> device,
/// waited for synchronously, every failure returned as text.

namespace xt::wgpu {

namespace {

std::mutex g_errorMutex;
std::string g_deviceErrors;  ///< uncaptured errors of the devices headless() created

void onUncapturedError(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* userdata1,
                       void* userdata2) {
    (void)device;
    (void)userdata1;
    (void)userdata2;
    std::lock_guard<std::mutex> lock(g_errorMutex);
    if (g_deviceErrors.size() < 4096) {
        if (!g_deviceErrors.empty()) {
            g_deviceErrors += "; ";
        }
        g_deviceErrors += "WebGPU error " + std::to_string(static_cast<int>(type)) + ": " + toString(message);
    }
}

void onDeviceLost(WGPUDevice const* device, WGPUDeviceLostReason reason, WGPUStringView message,
                  void* userdata1, void* userdata2) {
    (void)device;
    (void)userdata1;
    (void)userdata2;
    if (reason == WGPUDeviceLostReason_Destroyed || reason == WGPUDeviceLostReason_CallbackCancelled) {
        return;  // our own release / shutdown
    }
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_deviceErrors += "WebGPU device lost: " + toString(message);
}

/// The value of environment variable `name` ("" when unset).
std::string envString(const char* name) {
#if defined(_MSC_VER)
    size_t len = 0;
    char buf[64] = {0};
    if (getenv_s(&len, buf, sizeof(buf), name) != 0 || len == 0) {
        return std::string();
    }
    return std::string(buf);
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

struct AdapterRequest {
    bool done = false;
    WGPUAdapter adapter = nullptr;
    std::string message;
};

struct DeviceRequest {
    bool done = false;
    WGPUDevice device = nullptr;
    std::string message;
};

/// Calls wgpuInstanceProcessEvents until `*done` (at most ~10 s; wgpu-native normally answers
/// requestAdapter / requestDevice before returning).
void waitFor(WGPUInstance instance, const bool* done) {
    for (int i = 0; i < 10000 && !*done; ++i) {
        wgpuInstanceProcessEvents(instance);
        if (!*done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

WGPUAdapter requestAdapter(WGPUInstance instance, bool forceFallback, WGPUBackendType backend,
                           WGPUPowerPreference power, std::string& message) {
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = power;
    options.forceFallbackAdapter = forceFallback ? 1u : 0u;
    options.backendType = backend;
    AdapterRequest req;
    WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView msg, void* u1,
                     void*) {
        auto* r = static_cast<AdapterRequest*>(u1);
        if (status == WGPURequestAdapterStatus_Success) {
            r->adapter = adapter;
        } else {
            r->message = toString(msg);
            if (adapter != nullptr) {
                wgpuAdapterRelease(adapter);
            }
        }
        r->done = true;
    };
    cb.userdata1 = &req;
    wgpuInstanceRequestAdapter(instance, &options, cb);
    waitFor(instance, &req.done);
    if (!req.done) {
        message = "requestAdapter did not answer";
    } else if (req.adapter == nullptr) {
        message = req.message.empty() ? "no adapter" : req.message;
    }
    return req.adapter;
}

/// `name` (XT_WGPU_BACKEND, or WgpuHeadlessOptions::backend) -> instance backend mask; empty = the
/// primary backends; false for an unknown name.
bool backendFromName(const std::string& name, WGPUInstanceBackend& mask) {
    if (name.empty()) {
        mask = WGPUInstanceBackend_Primary;
        return true;
    }
    if (name == "vulkan") {
        mask = WGPUInstanceBackend_Vulkan;
    } else if (name == "d3d12") {
        mask = WGPUInstanceBackend_DX12;
    } else if (name == "metal") {
        mask = WGPUInstanceBackend_Metal;
    } else if (name == "gl") {
        mask = WGPUInstanceBackend_GL;
    } else {
        return false;
    }
    return true;
}

/// T-671 (50 §50.ab addendum 2, item 5): XT_WGPU_POWER=low|high -> powerPreference; unset = HighPerformance
/// as before. Any other value is ignored (today's behaviour) and said in `note`.
WGPUPowerPreference powerFromEnv(std::string& note) {
    const std::string value = envString("XT_WGPU_POWER");
    if (value == "low") {
        note = "power low";
        return WGPUPowerPreference_LowPower;
    }
    note = value == "high" ? "power high" : "power default (high)";
    if (!value.empty() && value != "high") {
        note += ", XT_WGPU_POWER=" + value + " ignored (not low or high)";
    }
    return WGPUPowerPreference_HighPerformance;
}

const char* backendTypeName(WGPUBackendType type) {
    switch (type) {
        case WGPUBackendType_D3D12:
            return "d3d12";
        case WGPUBackendType_D3D11:
            return "d3d11";
        case WGPUBackendType_Vulkan:
            return "vulkan";
        case WGPUBackendType_Metal:
            return "metal";
        case WGPUBackendType_OpenGL:
            return "gl";
        case WGPUBackendType_OpenGLES:
            return "gles";
        case WGPUBackendType_WebGPU:
            return "webgpu";
        case WGPUBackendType_Null:
            return "null";
        default:
            return "unknown";
    }
}

const char* adapterTypeName(WGPUAdapterType type) {
    switch (type) {
        case WGPUAdapterType_DiscreteGPU:
            return "discrete";
        case WGPUAdapterType_IntegratedGPU:
            return "integrated";
        case WGPUAdapterType_CPU:
            return "cpu";
        default:
            return "unknown";
    }
}

/// "wgpu-native a.b.c.d (source)": wgpuGetVersion and where the library came from (XT_WGPU_NATIVE_SOURCE,
/// Tools/CMake/xt_wgpu.cmake; a locally built wgpu-native reports 0.0.0.0, its COMMIT.txt names it).
std::string nativeVersionText() {
    const uint32_t v = wgpuGetVersion();
    std::string text = "wgpu-native " + std::to_string((v >> 24) & 0xFFu) + "." +
                       std::to_string((v >> 16) & 0xFFu) + "." + std::to_string((v >> 8) & 0xFFu) + "." +
                       std::to_string(v & 0xFFu);
#if defined(XT_WGPU_NATIVE_SOURCE)
    text += std::string(" (") + XT_WGPU_NATIVE_SOURCE + ")";
#endif
    return text;
}

#if defined(XT_WGPU_HAS_MEMORY_HINTS)
/// T-671 (50 §50.ab addendum 2, items 1 and 6): XT_WGPU_MEMORY_HINTS = performance | usage |
/// manual:<startMiB>:<maxMiB> into `extras`; unset = usage (the development default of an override build).
/// A malformed value falls back to usage, said in the returned text.
std::string memoryHintsFromEnv(WGPUDeviceExtras& extras) {
    const std::string value = envString("XT_WGPU_MEMORY_HINTS");
    extras.memoryHints = WGPUMemoryHints_MemoryUsage;
    if (value.empty()) {
        return "memory hints usage (default)";
    }
    if (value == "usage") {
        return "memory hints usage";
    }
    if (value == "performance") {
        extras.memoryHints = WGPUMemoryHints_Performance;
        return "memory hints performance";
    }
    uint32_t startMiB = 0;
    uint32_t maxMiB = 0;
    bool parsed = false;
    if (value.rfind("manual:", 0) == 0) {
        const char* end = value.data() + value.size();
        const auto a = std::from_chars(value.data() + 7, end, startMiB);
        if (a.ec == std::errc() && a.ptr != end && *a.ptr == ':') {
            const auto b = std::from_chars(a.ptr + 1, end, maxMiB);
            parsed = b.ec == std::errc() && b.ptr == end;
        }
    }
    if (parsed && startMiB >= 1 && maxMiB >= startMiB && maxMiB <= 65536) {
        extras.memoryHints = WGPUMemoryHints_Manual;
        extras.suballocatedDeviceMemoryBlockSizeStart = static_cast<uint64_t>(startMiB) << 20;
        extras.suballocatedDeviceMemoryBlockSizeEnd = static_cast<uint64_t>(maxMiB) << 20;
        return "memory hints manual " + std::to_string(startMiB) + ".." + std::to_string(maxMiB) + " MiB";
    }
    return "memory hints usage (default; XT_WGPU_MEMORY_HINTS=" + value +
           " is not performance, usage or manual:<startMiB>:<maxMiB>)";
}
#else
/// The pinned wgpu-native has no memory hints (built-in Performance): a set XT_WGPU_MEMORY_HINTS is ignored.
std::string memoryHintsNote() {
    const std::string value = envString("XT_WGPU_MEMORY_HINTS");
    std::string text = "memory hints performance (built in)";
    if (!value.empty()) {
        text += ", XT_WGPU_MEMORY_HINTS=" + value + " ignored (this wgpu-native has no memory hints)";
    }
    return text;
}
#endif

/// requestDevice with `limits` (null = wgpu-native's base limits, as before T-670), waited for.
/// Null and `message` on failure.
WGPUDevice requestDevice(WGPUInstance instance, WGPUAdapter adapter, const WGPULimits* limits,
                         WGPUChainedStruct* extras, std::string& message) {
    WGPUFeatureName features[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(adapter, WGPUFeatureName_TextureCompressionBC) != 0) {
        features[featureCount++] = WGPUFeatureName_TextureCompressionBC;
    }
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.label = WGPUStringView{"xt_wgpu headless", WGPU_STRLEN};
    dd.requiredFeatureCount = featureCount;
    dd.requiredFeatures = features;
    dd.requiredLimits = limits;
    dd.nextInChain = extras;  // T-671: WGPUDeviceExtras (memory hints) or null
    dd.uncapturedErrorCallbackInfo.callback = onUncapturedError;
    dd.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    dd.deviceLostCallbackInfo.callback = onDeviceLost;
    DeviceRequest req;
    WGPURequestDeviceCallbackInfo cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView msg, void* u1, void*) {
        auto* q = static_cast<DeviceRequest*>(u1);
        if (status == WGPURequestDeviceStatus_Success) {
            q->device = device;
        } else {
            q->message = toString(msg);
            if (device != nullptr) {
                wgpuDeviceRelease(device);
            }
        }
        q->done = true;
    };
    cb.userdata1 = &req;
    wgpuAdapterRequestDevice(adapter, &dd, cb);
    waitFor(instance, &req.done);
    if (req.device == nullptr) {
        message = req.done ? req.message : std::string("no answer");
    }
    return req.device;
}

/// One instance limited to `backends` -> adapter -> device, into `r`. On failure `r.error` says why
/// and whatever was created is released.
bool createOn(WgpuInit& r, WGPUInstanceBackend backends, const std::string& backendName, bool fromOptions,
              bool allowFallback) {
    r.notes.clear();  // T-671: a retry on other backends reports its own adapter
    WGPUInstanceExtras extras;
    std::memset(&extras, 0, sizeof extras);
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = backends;
    WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    desc.nextInChain = &extras.chain;
    r.instance = wgpuCreateInstance(&desc);
    if (r.instance == nullptr) {
        r.error = "wgpuCreateInstance failed";
        return false;
    }
    // §10.10.1: the platform's own API first (D3D12 / Metal / Vulkan), then any primary backend;
    // XT_WGPU_BACKEND already limited the instance to one.
#if defined(_WIN32)
    const WGPUBackendType preferred = backendName.empty() ? WGPUBackendType_D3D12 : WGPUBackendType_Undefined;
#elif defined(__APPLE__)
    const WGPUBackendType preferred = backendName.empty() ? WGPUBackendType_Metal : WGPUBackendType_Undefined;
#else
    const WGPUBackendType preferred =
        backendName.empty() ? WGPUBackendType_Vulkan : WGPUBackendType_Undefined;
#endif
    std::string message;
    std::string powerNote;
    const WGPUPowerPreference power = powerFromEnv(powerNote);
    for (int attempt = 0; attempt < 4 && r.adapter == nullptr; ++attempt) {
        const bool fallback = attempt < 2;
        if (fallback && !allowFallback) {
            continue;
        }
        const WGPUBackendType backend = (attempt % 2) == 0 ? preferred : WGPUBackendType_Undefined;
        if ((attempt % 2) == 1 && preferred == WGPUBackendType_Undefined) {
            continue;  // same request as the previous attempt
        }
        r.adapter = requestAdapter(r.instance, fallback, backend, power, message);
    }
    if (r.adapter == nullptr) {
        r.error =
            "no WebGPU adapter" +
            (backendName.empty() ? std::string()
                                 : (fromOptions ? " for backend " : " for XT_WGPU_BACKEND=") + backendName) +
            " (" + message + ")";
        r.release();
        return false;
    }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(r.adapter, &info) == WGPUStatus_Success) {
        const bool software = info.adapterType == WGPUAdapterType_CPU;
        r.notes = "adapter '" + toString(info.device) + "', " + backendTypeName(info.backendType) + ", " +
                  adapterTypeName(info.adapterType) + ", " + powerNote;
        wgpuAdapterInfoFreeMembers(info);
        if (software && !allowFallback) {
            r.error = "no WebGPU adapter (only a software adapter; set XT_WGPU_FALLBACK=1 to use it)";
            r.release();
            return false;
        }
    }

    // T-670 (50 §50.ab addendum 1 ii): a smaller D3D12 descriptor heap. Every other limit stays
    // WGPU_LIMIT_*_UNDEFINED, which wgpu-native replaces with the same adapter-derived base limits
    // it uses when `requiredLimits` is null (the pre-T-670 request).
    WGPUNativeLimits nativeLimits = WGPU_NATIVE_LIMITS_INIT;
    nativeLimits.maxNonSamplerBindings = kMaxNonSamplerBindings;
    WGPULimits limits = WGPU_LIMITS_INIT;
    limits.nextInChain = &nativeLimits.chain;
#if defined(XT_WGPU_HAS_MEMORY_HINTS)
    WGPUDeviceExtras memoryExtras;
    std::memset(&memoryExtras, 0, sizeof memoryExtras);
    memoryExtras.chain.sType = static_cast<WGPUSType>(WGPUSType_DeviceExtras);
    const std::string memoryNote = memoryHintsFromEnv(memoryExtras);
    WGPUChainedStruct* deviceExtras = &memoryExtras.chain;
#else
    const std::string memoryNote = memoryHintsNote();
    WGPUChainedStruct* deviceExtras = nullptr;
#endif
    r.notes += (r.notes.empty() ? "" : "\n") + nativeVersionText() + ", " + memoryNote;
    std::string deviceMessage;
    WGPUDevice device = requestDevice(r.instance, r.adapter, &limits, deviceExtras, deviceMessage);
    if (device == nullptr) {
        std::fprintf(stderr,
                     "xt::wgpu: requestDevice with maxNonSamplerBindings %u failed (%s); retrying with the "
                     "default limits\n",
                     kMaxNonSamplerBindings, deviceMessage.c_str());
        deviceMessage.clear();
        device = requestDevice(r.instance, r.adapter, nullptr, deviceExtras, deviceMessage);
    }
    if (device == nullptr) {
        r.error = "requestDevice failed (" + deviceMessage + ")";
        r.release();
        return false;
    }
    r.device = device;
    r.queue = wgpuDeviceGetQueue(r.device);
    return true;
}

}  // namespace

std::string takeDeviceErrors() {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    std::string out;
    out.swap(g_deviceErrors);
    return out;
}

bool planInstance(const std::string& backendName, bool windows, InstancePlan& plan) {
    plan = InstancePlan{};
    if (!backendFromName(backendName, plan.first)) {
        return false;
    }
    if (backendName.empty() && windows) {
        plan.first = WGPUInstanceBackend_DX12;  // T-670: no Vulkan loader / ICDs on the default path
        plan.retryPrimary = true;
    }
    return true;
}

WgpuInit WgpuInit::headless(const WgpuHeadlessOptions& options) {
    WgpuInit r;
    r.targetFormat = options.targetFormat;
    r.sampleCount = options.sampleCount;
    const bool fromOptions = !options.backend.empty();
    const std::string backendName = fromOptions ? options.backend : envString("XT_WGPU_BACKEND");
#if defined(_WIN32)
    constexpr bool kWindows = true;
#else
    constexpr bool kWindows = false;
#endif
    InstancePlan plan;
    if (!planInstance(backendName, kWindows, plan)) {
        r.error = (fromOptions ? "backend '" + backendName + "'" : "XT_WGPU_BACKEND=" + backendName) +
                  " is not one of vulkan, d3d12, metal, gl";
        return r;
    }
    const bool allowFallback = options.allowFallback || envString("XT_WGPU_FALLBACK") == "1";
    if (createOn(r, plan.first, backendName, fromOptions, allowFallback) || !plan.retryPrimary) {
        return r;
    }
    // T-670 (50 §50.ab addendum 1 i): no adapter or device on the D3D12-only instance (createOn
    // released it); the pre-T-670 Primary mask, once.
    std::fprintf(stderr,
                 "xt::wgpu: no adapter or device on the D3D12-only instance (%s); retrying with the primary "
                 "backends\n",
                 r.error.c_str());
    r.error.clear();
    createOn(r, WGPUInstanceBackend_Primary, backendName, fromOptions, allowFallback);
    return r;
}

void WgpuInit::release() {
    if (queue != nullptr) {
        wgpuQueueRelease(queue);
        queue = nullptr;
    }
    if (device != nullptr) {
        wgpuDeviceRelease(device);
        device = nullptr;
    }
    if (adapter != nullptr) {
        wgpuAdapterRelease(adapter);
        adapter = nullptr;
    }
    if (instance != nullptr) {
        wgpuInstanceRelease(instance);
        instance = nullptr;
    }
}

}  // namespace xt::wgpu
