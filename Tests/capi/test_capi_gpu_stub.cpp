// T-636 (Docs/design/50-c-abi.md §50.z): the GPU target of the C ABI in a library built without xt_wgpu
// (the XTFrame-Runtime mirror, XT_BUILD_WGPU=OFF): xt_gpu_target_create returns NULL and every other GPU
// target function XT_STATUS_UNSUPPORTED. T-689 (Docs/design/45-repository-layout.md §45.8): moved out of
// test_capi_gpu.cpp unchanged; Tests/capi/CMakeLists.txt compiles this file when the build has no xt_wgpu
// and test_capi_gpu.cpp / test_capi_gpu_text.cpp (exported with XTFrame-Backends) otherwise.

#include <cstdint>
#include <cstring>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

xt_gpu_target_desc offscreenDesc(int width, int height) {
    xt_gpu_target_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof(desc);
    desc.window.kind = XT_WINDOW_NONE;
    desc.width = width;
    desc.height = height;
    desc.vsync = 1;
    desc.backend = "";
    desc.sample_count = 4;
    return desc;
}

bool contains(const char* text, const char* part) {
    return text != nullptr && std::strstr(text, part) != nullptr;
}

}  // namespace

TEST_CASE("capi gpu 0 (T-636): a build without wgpu answers NULL / UNSUPPORTED") {
    const xt_gpu_target_desc desc = offscreenDesc(16, 16);
    CHECK(xt_gpu_target_create(&desc) == nullptr);
    CHECK(contains(xt_capi_last_error(), "built without wgpu"));
    CHECK(xt_gpu_target_begin_frame(nullptr, nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_end_frame(nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_resize(nullptr, 1, 1) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_readback(nullptr, nullptr, 0) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_destroy(nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_surface_render_gpu(nullptr, nullptr, nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_surface_detach_gpu(nullptr) == XT_STATUS_UNSUPPORTED);
    // T-656
    CHECK(xt_gpu_target_set_image_budget(nullptr, 0) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_trim(nullptr) == XT_STATUS_UNSUPPORTED);
    xt_gpu_memory_stats stats{};
    stats.struct_size = sizeof stats;
    CHECK(xt_gpu_target_memory_stats(nullptr, &stats) == XT_STATUS_UNSUPPORTED);
    // T-688 (50 §50.ae)
    uint32_t loaded = 7;
    CHECK(xt_gpu_target_preload_ui(nullptr, nullptr, "ui", &loaded) == XT_STATUS_UNSUPPORTED);
    CHECK(loaded == 0);
    CHECK(contains(xt_capi_last_error(), "built without wgpu"));
    loaded = 7;
    CHECK(xt_gpu_target_preload_image(nullptr, nullptr, "assets/a.png", &loaded) == XT_STATUS_UNSUPPORTED);
    CHECK(loaded == 0);
    CHECK(xt_gpu_target_preload_image(nullptr, nullptr, "assets/a.png", nullptr) == XT_STATUS_UNSUPPORTED);
}

TEST_CASE("capi gpu 0b (T-709): world layers without wgpu answer UNSUPPORTED; the camera helpers work") {
    CHECK(xt_surface_set_world_layers(nullptr, 1) == XT_STATUS_UNSUPPORTED);
    CHECK(contains(xt_capi_last_error(), "built without wgpu"));
    CHECK(xt_surface_set_world_layer_density(nullptr, 1.0f) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_surface_render_layers_gpu(nullptr, nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_surface_world_layer_count(nullptr) == 0u);
    xt_world_layer layer{};
    layer.struct_size = sizeof layer;
    CHECK(xt_surface_world_layer_info(nullptr, 0, &layer) == XT_STATUS_UNSUPPORTED);
    uint8_t px[4] = {};
    CHECK(xt_surface_world_layer_readback(nullptr, 0, px, 4) == XT_STATUS_UNSUPPORTED);
    float m[16] = {};
    CHECK(xt_surface_draw_layers_gpu(nullptr, nullptr, nullptr, m, 1.0f) == XT_STATUS_UNSUPPORTED);
    xt_input_event e{};
    e.struct_size = sizeof e;
    const float o[3] = {0, 0, 1}, d[3] = {0, 0, -1};
    CHECK(xt_surface_push_input_ray(nullptr, &e, o, d) == XT_STATUS_UNSUPPORTED);
    // 50 §50.af items 5-6: pure math, available in every build.
    CHECK(xt_world_layer_orbit_matrix(m, 1280, 720, 1280, 720, -30, 14, 0) == XT_STATUS_OK);
    float origin[3] = {}, direction[3] = {};
    CHECK(xt_world_layer_unproject(m, 1.0f, 640, 360, origin, direction) == XT_STATUS_OK);
    CHECK(direction[2] < 0.0f);
}
