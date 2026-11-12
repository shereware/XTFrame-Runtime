#include <string>

#include "capi_internal.h"

/// `xt_gpu_target_*`, `xt_surface_render_gpu`, `xt_surface_detach_gpu` in a build without wgpu
/// (Docs/design/50-c-abi.md §50.z, Docs/tasks/T-636.md): every function only reports "built without wgpu"
/// (`xt_gpu_target_create` returns NULL, the others XT_STATUS_UNSUPPORTED).
/// Docs/design/45-repository-layout.md §45.8 item 2 (T-689): moved out of capi_gpu.cpp unchanged;
/// Code/Runtime/capi/CMakeLists.txt compiles this file when the build has no xt_wgpu (the XTFrame-Runtime
/// mirror, XT_BUILD_WGPU=OFF) and capi_gpu.cpp, the GPU target over xt_wgpu, otherwise.

using xt::capi::fail;

/// Never created: xt_gpu_target_create fails in a build without wgpu.
struct xt_gpu_target {};

namespace xt::capi {

struct SurfaceGpu {};

void SurfaceGpuDeleter::operator()(SurfaceGpu* gpu) const noexcept { delete gpu; }

void detachGpu(xt_surface& surface) {
    surface.gpu.reset();
    surface.gpuTarget = nullptr;
    surface.renderPath = RenderPath::None;
}

void releaseGpuResources(xt_surface& surface, bool /*everything*/) { surface.gpu.reset(); }

void releaseGpuPoolUse(xt_surface& /*surface*/) {}

bool gpuDrawCounts(const xt_surface& /*surface*/, uint32_t& /*executed*/, uint32_t& /*skipped*/) {
    return false;
}

}  // namespace xt::capi

namespace {

xt_status withoutWgpu(const char* function) {
    return fail(XT_STATUS_UNSUPPORTED, std::string(function) + ": built without wgpu");
}

}  // namespace

extern "C" {

XT_CAPI xt_gpu_target* xt_gpu_target_create(const xt_gpu_target_desc* /*desc*/) {
    XT_CAPI_GUARD_OR("xt_gpu_target_create", xt_gpu_target*, nullptr, {
        xt::capi::setLastError("xt_gpu_target_create: built without wgpu");
        return nullptr;
    });
}

XT_CAPI xt_status xt_gpu_target_destroy(xt_gpu_target* /*t*/) {
    XT_CAPI_GUARD("xt_gpu_target_destroy", { return withoutWgpu("xt_gpu_target_destroy"); });
}

XT_CAPI xt_status xt_gpu_target_resize(xt_gpu_target* /*t*/, int /*width*/, int /*height*/) {
    XT_CAPI_GUARD("xt_gpu_target_resize", { return withoutWgpu("xt_gpu_target_resize"); });
}

XT_CAPI xt_status xt_gpu_target_begin_frame(xt_gpu_target* /*t*/, const float /*clear_rgba*/[4]) {
    XT_CAPI_GUARD("xt_gpu_target_begin_frame", { return withoutWgpu("xt_gpu_target_begin_frame"); });
}

XT_CAPI xt_status xt_gpu_target_end_frame(xt_gpu_target* /*t*/) {
    XT_CAPI_GUARD("xt_gpu_target_end_frame", { return withoutWgpu("xt_gpu_target_end_frame"); });
}

XT_CAPI xt_status xt_gpu_target_readback(xt_gpu_target* /*t*/, void* /*rgba*/, int /*stride*/) {
    XT_CAPI_GUARD("xt_gpu_target_readback", { return withoutWgpu("xt_gpu_target_readback"); });
}

XT_CAPI xt_status xt_surface_render_gpu(xt_surface* /*s*/, xt_gpu_target* /*t*/, const xt_rect* /*dest*/) {
    XT_CAPI_GUARD("xt_surface_render_gpu", { return withoutWgpu("xt_surface_render_gpu"); });
}

XT_CAPI xt_status xt_surface_detach_gpu(xt_surface* /*s*/) {
    XT_CAPI_GUARD("xt_surface_detach_gpu", { return withoutWgpu("xt_surface_detach_gpu"); });
}

XT_CAPI xt_status xt_gpu_target_set_image_budget(xt_gpu_target* /*t*/, uint64_t /*idle_bytes*/) {
    XT_CAPI_GUARD("xt_gpu_target_set_image_budget",
                  { return withoutWgpu("xt_gpu_target_set_image_budget"); });
}

XT_CAPI xt_status xt_gpu_target_trim(xt_gpu_target* /*t*/) {
    XT_CAPI_GUARD("xt_gpu_target_trim", { return withoutWgpu("xt_gpu_target_trim"); });
}

XT_CAPI xt_status xt_gpu_target_memory_stats(const xt_gpu_target* /*t*/, xt_gpu_memory_stats* /*out*/) {
    XT_CAPI_GUARD("xt_gpu_target_memory_stats", { return withoutWgpu("xt_gpu_target_memory_stats"); });
}

XT_CAPI xt_status xt_gpu_target_preload_ui(xt_gpu_target* /*t*/, xt_bundle* /*b*/, const char* /*ui_id*/,
                                           uint32_t* out_loaded) {
    XT_CAPI_GUARD("xt_gpu_target_preload_ui", {
        if (out_loaded != nullptr) *out_loaded = 0;
        return withoutWgpu("xt_gpu_target_preload_ui");
    });
}

XT_CAPI xt_status xt_gpu_target_preload_image(xt_gpu_target* /*t*/, xt_bundle* /*b*/, const char* /*asset*/,
                                              uint32_t* out_loaded) {
    XT_CAPI_GUARD("xt_gpu_target_preload_image", {
        if (out_loaded != nullptr) *out_loaded = 0;
        return withoutWgpu("xt_gpu_target_preload_image");
    });
}

}  // extern "C"
