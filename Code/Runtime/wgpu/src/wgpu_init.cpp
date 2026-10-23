#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <chrono>
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
                           std::string& message) {
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
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

/// XT_WGPU_BACKEND -> instance backend mask; false for an unknown name.
bool backendFromEnv(WGPUInstanceBackend& mask, std::string& name) {
    name = envString("XT_WGPU_BACKEND");
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

}  // namespace

std::string takeDeviceErrors() {
    std::lock_guard<std::mutex> lock(g_errorMutex);
    std::string out;
    out.swap(g_deviceErrors);
    return out;
}

WgpuInit WgpuInit::headless(const WgpuHeadlessOptions& options) {
    WgpuInit r;
    r.targetFormat = options.targetFormat;
    r.sampleCount = options.sampleCount;
    WGPUInstanceBackend backends = WGPUInstanceBackend_Primary;
    std::string backendName;
    if (!backendFromEnv(backends, backendName)) {
        r.error = "XT_WGPU_BACKEND=" + backendName + " is not one of vulkan, d3d12, metal, gl";
        return r;
    }
    WGPUInstanceExtras extras;
    std::memset(&extras, 0, sizeof extras);
    extras.chain.sType = static_cast<WGPUSType>(WGPUSType_InstanceExtras);
    extras.backends = backends;
    WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;
    desc.nextInChain = &extras.chain;
    r.instance = wgpuCreateInstance(&desc);
    if (r.instance == nullptr) {
        r.error = "wgpuCreateInstance failed";
        return r;
    }
    const bool allowFallback = options.allowFallback || envString("XT_WGPU_FALLBACK") == "1";
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
    for (int attempt = 0; attempt < 4 && r.adapter == nullptr; ++attempt) {
        const bool fallback = attempt < 2;
        if (fallback && !allowFallback) {
            continue;
        }
        const WGPUBackendType backend = (attempt % 2) == 0 ? preferred : WGPUBackendType_Undefined;
        if ((attempt % 2) == 1 && preferred == WGPUBackendType_Undefined) {
            continue;  // same request as the previous attempt
        }
        r.adapter = requestAdapter(r.instance, fallback, backend, message);
    }
    if (r.adapter == nullptr) {
        r.error = "no WebGPU adapter" +
                  (backendName.empty() ? std::string() : " for XT_WGPU_BACKEND=" + backendName) + " (" +
                  message + ")";
        r.release();
        return r;
    }
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    if (wgpuAdapterGetInfo(r.adapter, &info) == WGPUStatus_Success) {
        const bool software = info.adapterType == WGPUAdapterType_CPU;
        wgpuAdapterInfoFreeMembers(info);
        if (software && !allowFallback) {
            r.error = "no WebGPU adapter (only a software adapter; set XT_WGPU_FALLBACK=1 to use it)";
            r.release();
            return r;
        }
    }

    WGPUFeatureName features[1];
    size_t featureCount = 0;
    if (wgpuAdapterHasFeature(r.adapter, WGPUFeatureName_TextureCompressionBC) != 0) {
        features[featureCount++] = WGPUFeatureName_TextureCompressionBC;
    }
    WGPUDeviceDescriptor dd = WGPU_DEVICE_DESCRIPTOR_INIT;
    dd.label = WGPUStringView{"xt_wgpu headless", WGPU_STRLEN};
    dd.requiredFeatureCount = featureCount;
    dd.requiredFeatures = features;
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
    wgpuAdapterRequestDevice(r.adapter, &dd, cb);
    waitFor(r.instance, &req.done);
    if (req.device == nullptr) {
        r.error = "requestDevice failed (" + (req.done ? req.message : std::string("no answer")) + ")";
        r.release();
        return r;
    }
    r.device = req.device;
    r.queue = wgpuDeviceGetQueue(r.device);
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
