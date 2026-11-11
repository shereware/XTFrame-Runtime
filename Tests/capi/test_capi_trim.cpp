// T-641 (Docs/design/50-c-abi.md §50.aa): a hidden surface releases its renderer scratch --
// xt_surface_set_trim_on_hide (default 1), xt_surface_trim. The heavy fixture is game-ui's `pause`
// (four animated glow layers; T-640 measured a 116 MB step per surface at 1280x720). Each case
// renders it for kFrames frames, hides it and runs two updates, then shows it again: the frame after
// re-showing must equal, byte for byte, the frame before hiding (every update while hidden is dt 0,
// so the clock and the animation stay where they were). The private-byte checks run on Windows only
// (GetProcessMemoryInfo); elsewhere only the pixels are checked.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// clang-format off
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
// clang-format on
#pragma comment(lib, "dxgi.lib")
#endif

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;
/// Frames rendered before hiding (50 §50.aa test plan: 200). A Debug SW frame of `pause` is tens of
/// times slower than a Release one, so a Debug build renders fewer; the pool is warm either way.
#ifdef NDEBUG
constexpr int kFrames = 200;
#else
constexpr int kFrames = 24;
#endif
constexpr float kDt = 1.0f / 60.0f;

/// The process's private bytes (Windows); 0 elsewhere (the memory checks are then skipped).
double privateMb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc))) {
        return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

/// This process's video memory (local + non-local, every adapter; Windows/WDDM), in MB; 0 elsewhere.
/// Only the Windows GPU cases call it, hence [[maybe_unused]] (T-643: GCC -Werror=unused-function).
[[maybe_unused]] double videoMb() {
#ifdef _WIN32
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return 0.0;
    double total = 0.0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        IDXGIAdapter3* a3 = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
                total += static_cast<double>(info.CurrentUsage);
            if (SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
                total += static_cast<double>(info.CurrentUsage);
            a3->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return total / (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}

using Pixels = std::vector<uint8_t>;

/// The game-ui bundle and one 1280x720 `pause` surface; destroyed / closed at scope end.
struct PauseSurface {
    xt_bundle* b = nullptr;
    xt_surface* s = nullptr;
    double baseMb = 0.0;  ///< private bytes just before xt_surface_create
    Pixels rgba = Pixels(static_cast<size_t>(kW) * kH * 4u, 0);
    PauseSurface() {
        b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
        REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
        xt_surface_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.width = kW;
        desc.height = kH;
        desc.device_pixel_ratio = 1.0f;
        desc.premultiplied = 1;
        baseMb = privateMb();
        s = xt_surface_create(b, "pause", &desc);
        REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    }
    ~PauseSurface() {
        if (s != nullptr) xt_surface_destroy(s);
        if (b != nullptr) (void)xt_bundle_close(b);
    }
    PauseSurface(const PauseSurface&) = delete;
    PauseSurface& operator=(const PauseSurface&) = delete;

    void frame(float dt, xt_damage* damage = nullptr) {
        REQUIRE(xt_surface_update(s, dt) == XT_STATUS_OK);
        REQUIRE(xt_surface_render(s, rgba.data(), kW * 4, damage) == XT_STATUS_OK);
    }
    void warm() {
        for (int i = 0; i < kFrames; ++i) frame(kDt);
    }
};

bool samePixels(const Pixels& a, const Pixels& b) { return a.size() == b.size() && a == b; }

/// The released share of a surface's step: (before - after) / (before - base); 1 when not measured.
double releasedShare(double baseMb, double beforeMb, double afterMb) {
    std::printf("private MB: base %.1f, rendered %.1f, hidden %.1f (step %.1f, released %.1f)\n", baseMb,
                beforeMb, afterMb, beforeMb - baseMb, beforeMb - afterMb);
    if (baseMb <= 0.0 || beforeMb <= baseMb) return 1.0;
    return (beforeMb - afterMb) / (beforeMb - baseMb);
}

/// The update hook that trims the surface it runs on.
struct TrimProbe {
    xt_status status = XT_STATUS_INTERNAL;
    int calls = 0;
    static void onUpdate(xt_surface* s, float /*dt*/, void* user) {
        auto* self = static_cast<TrimProbe*>(user);
        ++self->calls;
        self->status = xt_surface_trim(s);
    }
};

}  // namespace

TEST_CASE("capi trim 1 (T-641): hiding releases >= 80 % of the surface's step; re-show is byte-identical") {
    PauseSurface f;
    f.warm();
    const Pixels before = f.rgba;
    const double renderedMb = privateMb();
    REQUIRE(xt_surface_set_visible(f.s, 0) == XT_STATUS_OK);
    f.frame(0.0f);
    f.frame(0.0f);
    const double hiddenMb = privateMb();
    CHECK(releasedShare(f.baseMb, renderedMb, hiddenMb) >= 0.8);
    REQUIRE(xt_surface_set_visible(f.s, 1) == XT_STATUS_OK);
    f.frame(0.0f);
    CHECK(samePixels(f.rgba, before));
    // Hidden again: released again (once per hide).
    REQUIRE(xt_surface_set_visible(f.s, 0) == XT_STATUS_OK);
    f.frame(0.0f);
    f.frame(0.0f);
    CHECK(releasedShare(f.baseMb, renderedMb, privateMb()) >= 0.8);
}

TEST_CASE("capi trim 2 (T-641): trim_on_hide 0 keeps the scratch while hidden") {
    PauseSurface f;
    CHECK(xt_surface_set_trim_on_hide(nullptr, 0) == XT_STATUS_INVALID_ARGUMENT);
    REQUIRE(xt_surface_set_trim_on_hide(f.s, 0) == XT_STATUS_OK);
    f.warm();
    const Pixels before = f.rgba;
    const double renderedMb = privateMb();
    REQUIRE(xt_surface_set_visible(f.s, 0) == XT_STATUS_OK);
    f.frame(0.0f);
    f.frame(0.0f);
#ifdef _WIN32
    // privateMb() measures on Windows only (0 elsewhere, where releasedShare answers 1).
    CHECK(releasedShare(f.baseMb, renderedMb, privateMb()) < 0.2);
#else
    (void)renderedMb;
#endif
    REQUIRE(xt_surface_set_visible(f.s, 1) == XT_STATUS_OK);
    f.frame(0.0f);
    CHECK(samePixels(f.rgba, before));
}

TEST_CASE("capi trim 3 (T-641): xt_surface_trim inside the update hook, and on a visible surface") {
    PauseSurface f;
    CHECK(xt_surface_trim(nullptr) == XT_STATUS_INVALID_ARGUMENT);
    f.warm();
    const Pixels before = f.rgba;
    const double renderedMb = privateMb();
    TrimProbe probe;
    REQUIRE(xt_surface_set_update_hook(f.s, &TrimProbe::onUpdate, &probe) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(f.s, 0.0f) == XT_STATUS_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.status == XT_STATUS_OK);
    REQUIRE(xt_surface_set_update_hook(f.s, nullptr, nullptr) == XT_STATUS_OK);
    CHECK(releasedShare(f.baseMb, renderedMb, privateMb()) >= 0.8);
    // Visible: the next frame repaints whole and draws the same pixels.
    xt_damage damage{};
    REQUIRE(xt_surface_render(f.s, f.rgba.data(), kW * 4, &damage) == XT_STATUS_OK);
    CHECK(damage.full == 1);
    CHECK(samePixels(f.rgba, before));
    // Outside any callback too.
    REQUIRE(xt_surface_trim(f.s) == XT_STATUS_OK);
    f.frame(0.0f, &damage);
    CHECK(damage.full == 1);
    CHECK(samePixels(f.rgba, before));
}

#ifdef XT_CAPI_TEST_HAS_WGPU

namespace {

xt_gpu_target* offscreenTarget() {
    xt_gpu_target_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof(desc);
    desc.window.kind = XT_WINDOW_NONE;
    desc.width = kW;
    desc.height = kH;
    desc.vsync = 1;
    desc.backend = "";
    desc.sample_count = 4;  // T-666: written when 0 meant 4; keeps its 4x target
    xt_gpu_target* t = xt_gpu_target_create(&desc);
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        REQUIRE_MESSAGE(error.find("no WebGPU adapter") != std::string::npos, error);
        std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
    }
    return t;
}

/// One GPU frame of `s` over the whole target, then the target read back.
Pixels gpuFrame(xt_surface* s, xt_gpu_target* t, float dt) {
    REQUIRE(xt_surface_update(s, dt) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
    const xt_rect dest{0, 0, kW, kH};
    REQUIRE_MESSAGE(xt_surface_render_gpu(s, t, &dest) == XT_STATUS_OK, xt_capi_last_error());
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
    Pixels px(static_cast<size_t>(kW) * kH * 4u);
    REQUIRE_MESSAGE(xt_gpu_target_readback(t, px.data(), kW * 4) == XT_STATUS_OK, xt_capi_last_error());
    return px;
}

}  // namespace

namespace {

/// Counts the info lines the GPU release / rebuild log (xt_capi_set_log); removed at scope end.
struct GpuLog {
    int released = 0;
    int rebuilt = 0;
    int returned = 0;  ///< T-675: trim-on-hide gave the surface's textures back (renderer kept)
    GpuLog() { xt_capi_set_log(&GpuLog::onLog, this); }
    ~GpuLog() { xt_capi_set_log(nullptr, nullptr); }
    GpuLog(const GpuLog&) = delete;
    GpuLog& operator=(const GpuLog&) = delete;
    static void onLog(int level, const char* message, void* user) {
        auto* self = static_cast<GpuLog*>(user);
        if (level == 0 && std::strstr(message, "GPU resources released") != nullptr) ++self->released;
        if (level == 0 && std::strstr(message, "GPU resources rebuilt") != nullptr) ++self->rebuilt;
        if (level == 0 && std::strstr(message, "GPU textures returned to the target") != nullptr)
            ++self->returned;
    }
};

/// `pause` on `t` for kFrames frames, hidden for two (and rendered: the host keeps compositing it),
/// then shown again: the frame after re-showing equals the one before hiding.
void gpuHideAndShow(xt_gpu_target* t, bool trimOnHide) {
    GpuLog log;
    PauseSurface f;
    REQUIRE(xt_surface_set_trim_on_hide(f.s, trimOnHide ? 1 : 0) == XT_STATUS_OK);
    const double v0 = videoMb();
    for (int i = 0; i + 1 < kFrames; ++i) (void)gpuFrame(f.s, t, kDt);
    const Pixels before = gpuFrame(f.s, t, kDt);
    const double renderedMb = privateMb();
    const double v1 = videoMb();
    REQUIRE(xt_surface_set_visible(f.s, 0) == XT_STATUS_OK);
    (void)gpuFrame(f.s, t, 0.0f);
    const Pixels hidden = gpuFrame(f.s, t, 0.0f);
    std::printf("gpu (trim_on_hide %d): video MB %.1f -> %.1f -> %.1f; private MB %.1f -> %.1f -> %.1f\n",
                trimOnHide ? 1 : 0, v0, v1, videoMb(), f.baseMb, renderedMb, privateMb());
    CHECK(hidden == Pixels(hidden.size(), 0));  // A hidden surface composites nothing.
    // T-675 (50 §50.ac item 2): trim-on-hide gives back only the textures; the renderer stays.
    CHECK(log.returned == (trimOnHide ? 1 : 0));
    CHECK(log.released == 0);
    CHECK(log.rebuilt == 0);
    // Still attached: the SW path is refused, the target cannot be destroyed.
    CHECK(xt_surface_render(f.s, f.rgba.data(), kW * 4, nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_BUSY);
    REQUIRE(xt_surface_set_visible(f.s, 1) == XT_STATUS_OK);
    CHECK(samePixels(gpuFrame(f.s, t, 0.0f), before));
    CHECK(log.rebuilt == 0);  // T-675: shown again with the renderer it kept
    // xt_surface_trim on the visible attached surface: everything released now, rebuilt by the next
    // frame.
    REQUIRE(xt_surface_trim(f.s) == XT_STATUS_OK);
    CHECK(log.released == 1);
    CHECK(samePixels(gpuFrame(f.s, t, 0.0f), before));
    CHECK(log.rebuilt == 1);
}

}  // namespace

// The GPU resources live in the target's WebGPU device, whose allocator keeps its memory blocks for
// the device's lifetime: process video memory and private bytes drop only with
// xt_gpu_target_destroy (measured on D3D12 and Vulkan; xt_surface_detach_gpu and xt_surface_destroy
// alike), and what a release frees is reused by the next resources on that target. So this case
// checks the release itself (the info log), that the surface stays attached, and the re-show pixels;
// the memory figures are printed.
TEST_CASE(
    "capi trim 4 (T-641, T-675): a hidden GPU-attached surface gives back its textures, stays attached") {
    xt_gpu_target* t = offscreenTarget();
    if (t == nullptr) return;
    gpuHideAndShow(t, true);
    gpuHideAndShow(t, false);
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
}

#endif  // XT_CAPI_TEST_HAS_WGPU
