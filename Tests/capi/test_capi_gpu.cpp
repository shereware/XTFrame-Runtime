// T-636 (Docs/design/50-c-abi.md §50.z): the GPU target of the C ABI -- xt_gpu_target_* and
// xt_surface_render_gpu / xt_surface_detach_gpu -- on an offscreen (XT_WINDOW_NONE) target read back
// with xt_gpu_target_readback. Each composited region is compared with the same UI rendered by
// xt_surface_render (premultiplied) at 15 §15.3's GPU tolerance (channel 16, 3 % of the pixels; as
// Tests/wgpu). Fixtures: Tests/capi/fixtures/state (64x64, four rows) and fixtures/flip (a red card).
// XT_CAPI_TEST_HAS_WGPU is set when the library was built with xt_wgpu; without it only the
// "built without wgpu" answers are checked. With wgpu but no adapter the cases print SKIP and pass.
// T-648 (cases 8-10, at sample counts 1 and 4): the target's shared image textures and pipeline set,
// observed through its info log lines (npdo `settings`, two surfaces of one bundle).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

#if defined(_WIN32) && defined(XT_CAPI_TEST_HAS_WGPU)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
    return desc;
}

bool contains(const char* text, const char* part) {
    return text != nullptr && std::strstr(text, part) != nullptr;
}

}  // namespace

#ifndef XT_CAPI_TEST_HAS_WGPU

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
}

#else  // XT_CAPI_TEST_HAS_WGPU

namespace {

using Pixels = std::vector<uint8_t>;

/// An offscreen target, or null when this machine has no WebGPU adapter (the case is skipped).
xt_gpu_target* makeTarget(int width, int height) {
    const xt_gpu_target_desc desc = offscreenDesc(width, height);
    xt_gpu_target* t = xt_gpu_target_create(&desc);
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
        std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
    }
    return t;
}

/// A bundle and the surfaces of one case; everything is destroyed at scope end (surfaces, then the
/// targets, then the bundles).
struct Fixture {
    std::vector<xt_bundle*> bundles;
    std::vector<xt_surface*> surfaces;
    std::vector<xt_gpu_target*> targets;
    ~Fixture() {
        for (xt_surface* s : surfaces) xt_surface_destroy(s);
        for (xt_gpu_target* t : targets) (void)xt_gpu_target_destroy(t);
        for (xt_bundle* b : bundles) (void)xt_bundle_close(b);
    }
    xt_surface* surface(const char* dir, const char* ui, int size, int premultiplied = 1) {
        xt_bundle* b = xt_bundle_open(dir);
        REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
        bundles.push_back(b);
        xt_surface_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.width = size;
        desc.height = size;
        desc.device_pixel_ratio = 1.0f;
        desc.premultiplied = premultiplied;
        xt_surface* s = xt_surface_create(b, ui, &desc);
        REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
        surfaces.push_back(s);
        return s;
    }
    xt_gpu_target* target(int width, int height) {
        xt_gpu_target* t = makeTarget(width, height);
        if (t != nullptr) targets.push_back(t);
        return t;
    }
    /// Destroys `s` now (it is not destroyed again at scope end).
    void destroy(xt_surface* s) {
        xt_surface_destroy(s);
        surfaces.erase(std::find(surfaces.begin(), surfaces.end(), s));
    }
    void forget(xt_gpu_target* t) { targets.erase(std::find(targets.begin(), targets.end(), t)); }
};

xt_surface* stateSurface(Fixture& f) { return f.surface(XT_CAPI_TEST_STATE_BUNDLE, "state", 64); }
xt_surface* flipSurface(Fixture& f, int size) { return f.surface(XT_CAPI_TEST_FLIP_BUNDLE, "flip", size); }

/// The SW reference of a surface after one update(0): a premultiplied `size` x `size` frame.
Pixels swFrame(xt_surface* s, int size) {
    Pixels px(static_cast<size_t>(size) * static_cast<size_t>(size) * 4u, 0);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(xt_surface_render(s, px.data(), size * 4, nullptr) == XT_STATUS_OK);
    return px;
}

Pixels readback(xt_gpu_target* t, int width, int height, int stride) {
    Pixels px(static_cast<size_t>(stride) * static_cast<size_t>(height), 0xCD);
    REQUIRE_MESSAGE(xt_gpu_target_readback(t, px.data(), stride) == XT_STATUS_OK, xt_capi_last_error());
    // Compact to width * 4 per row.
    Pixels out(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (int y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
                    px.data() + static_cast<size_t>(y) * static_cast<size_t>(stride),
                    static_cast<size_t>(width) * 4u);
    }
    return out;
}

const uint8_t* at(const Pixels& px, int width, int x, int y) {
    return px.data() + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
}

/// The `ref` (refW x refH) against the region of `gpu` (gpuW wide) at (dx, dy): 15 §15.3's GPU / SW
/// tolerance (a channel differing by more than 16 marks the pixel; at most 3 % of the pixels).
bool regionMatches(const Pixels& gpu, int gpuW, int dx, int dy, const Pixels& ref, int refW, int refH,
                   const char* what) {
    int diffPixels = 0, maxDiff = 0;
    for (int y = 0; y < refH; ++y) {
        for (int x = 0; x < refW; ++x) {
            const uint8_t* a = at(gpu, gpuW, dx + x, dy + y);
            const uint8_t* b = at(ref, refW, x, y);
            int d = 0;
            for (int c = 0; c < 4; ++c)
                d = std::max(d, std::abs(static_cast<int>(a[c]) - static_cast<int>(b[c])));
            maxDiff = std::max(maxDiff, d);
            if (d > 16) ++diffPixels;
        }
    }
    std::printf("gpu vs sw %s: %d px differ, max channel diff %d\n", what, diffPixels, maxDiff);
    return static_cast<float>(diffPixels) <= 0.03f * static_cast<float>(refW * refH);
}

/// `src` (n x n) scaled to (m x m) by bilinear sampling at pixel centres, clamped at the edges (what a
/// LinearClamp sampler does).
Pixels upscale(const Pixels& src, int n, int m) {
    Pixels out(static_cast<size_t>(m) * static_cast<size_t>(m) * 4u);
    const float scale = static_cast<float>(n) / static_cast<float>(m);
    for (int y = 0; y < m; ++y) {
        const float sy =
            std::clamp((static_cast<float>(y) + 0.5f) * scale - 0.5f, 0.0f, static_cast<float>(n - 1));
        const int y0 = static_cast<int>(sy), y1 = std::min(y0 + 1, n - 1);
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < m; ++x) {
            const float sx =
                std::clamp((static_cast<float>(x) + 0.5f) * scale - 0.5f, 0.0f, static_cast<float>(n - 1));
            const int x0 = static_cast<int>(sx), x1 = std::min(x0 + 1, n - 1);
            const float fx = sx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                const float top = at(src, n, x0, y0)[c] * (1 - fx) + at(src, n, x1, y0)[c] * fx;
                const float bottom = at(src, n, x0, y1)[c] * (1 - fx) + at(src, n, x1, y1)[c] * fx;
                out[(static_cast<size_t>(y) * static_cast<size_t>(m) + static_cast<size_t>(x)) * 4u +
                    static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(top * (1 - fy) + bottom * fy));
            }
        }
    }
    return out;
}

/// One frame of `t`: clear to transparent, then each (surface, dest) in order.
void frame(xt_gpu_target* t, std::initializer_list<std::pair<xt_surface*, xt_rect> > draws) {
    REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
    for (const auto& d : draws) {
        REQUIRE(xt_surface_update(d.first, 0.0f) == XT_STATUS_OK);
        REQUIRE_MESSAGE(xt_surface_render_gpu(d.first, t, &d.second) == XT_STATUS_OK, xt_capi_last_error());
    }
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
}

bool transparent(const Pixels& px, int width, int x, int y) {
    const uint8_t* p = at(px, width, x, y);
    return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0;
}

}  // namespace

TEST_CASE("capi gpu 1 (T-636): two surfaces composed at dest rects match their own SW renders") {
    Fixture f;
    xt_gpu_target* t = f.target(160, 80);
    if (t == nullptr) return;
    const Pixels stateRef = swFrame(stateSurface(f), 64);
    const Pixels flipRef = swFrame(flipSurface(f, 32), 32);
    xt_surface* state = stateSurface(f);
    xt_surface* flip = flipSurface(f, 32);
    xt_surface_set_perf_enabled(state, 1);
    frame(t, {{state, xt_rect{8, 8, 64, 64}}, {flip, xt_rect{100, 20, 32, 32}}});
    const Pixels px = readback(t, 160, 80, 160 * 4 + 12);  // a padded stride
    CHECK(regionMatches(px, 160, 8, 8, stateRef, 64, 64, "state @(8,8)"));
    CHECK(regionMatches(px, 160, 100, 20, flipRef, 32, 32, "flip @(100,20)"));
    CHECK(transparent(px, 160, 2, 2));  // outside both: the clear colour
    CHECK(transparent(px, 160, 150, 70));
    CHECK(at(px, 160, 116, 36)[0] == 255);  // the red card, premultiplied opaque
    CHECK(at(px, 160, 116, 36)[3] == 255);
    xt_frame_stats stats{};
    REQUIRE(xt_surface_frame_stats(state, &stats) == XT_STATUS_OK);
    CHECK(stats.render_ms >= 0.0f);
    CHECK(stats.executed_draws > 0);

    // Call order is paint order: the later surface is drawn over the earlier one.
    int x = -1, y = -1;
    for (int yy = 0; yy < 64 && x < 0; ++yy) {
        for (int xx = 0; xx < 64; ++xx) {
            if (at(stateRef, 64, xx, yy)[3] == 255 && at(stateRef, 64, xx, yy)[0] < 128) {
                x = xx;
                y = yy;
                break;
            }
        }
    }
    REQUIRE_MESSAGE(x >= 0, "the state fixture has no opaque, non-red pixel");
    const xt_rect card{std::max(0, x - 16), std::max(0, y - 16), 32, 32};  // over (x, y)
    frame(t, {{state, xt_rect{0, 0, 64, 64}}, {flip, card}});
    Pixels over = readback(t, 160, 80, 160 * 4);
    CHECK(at(over, 160, x, y)[0] == 255);  // flip (red) on top
    frame(t, {{flip, card}, {state, xt_rect{0, 0, 64, 64}}});
    over = readback(t, 160, 80, 160 * 4);
    CHECK(std::abs(static_cast<int>(at(over, 160, x, y)[0]) - static_cast<int>(at(stateRef, 64, x, y)[0])) <=
          16);

    // dest NULL = the whole target (the 32x32 card stretched over 160x80: red everywhere).
    REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_surface_render_gpu(flip, t, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
    over = readback(t, 160, 80, 160 * 4);
    CHECK(at(over, 160, 1, 1)[0] == 255);
    CHECK(at(over, 160, 158, 78)[0] == 255);
}

TEST_CASE("capi gpu 2 (T-636): a dest larger than the surface scales it linearly") {
    Fixture f;
    xt_gpu_target* t = f.target(128, 128);
    if (t == nullptr) return;
    const Pixels ref = upscale(swFrame(stateSurface(f), 64), 64, 128);
    xt_surface* state = stateSurface(f);
    const float clear[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    REQUIRE(xt_gpu_target_begin_frame(t, clear) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(state, 0.0f) == XT_STATUS_OK);
    REQUIRE(xt_surface_render_gpu(state, t, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
    // Over opaque blue: compare only where the surface is opaque (its own colour shows unblended).
    const Pixels px = readback(t, 128, 128, 128 * 4);
    Pixels masked = px;
    for (size_t i = 0; i < ref.size(); i += 4) {
        if (ref[i + 3] != 255) std::memcpy(&masked[i], &ref[i], 4);
    }
    CHECK(regionMatches(masked, 128, 0, 0, ref, 128, 128, "state x2"));
    // A transparent part shows the clear colour.
    bool sawBlue = false;
    for (size_t i = 0; i < ref.size() && !sawBlue; i += 4) {
        sawBlue = ref[i + 3] == 0 && px[i + 2] == 255 && px[i + 3] == 255;
    }
    CHECK(sawBlue);

    // Resize between frames, then the same surface again at 1:1 in the new size.
    CHECK(xt_gpu_target_resize(t, 64, 64) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
    CHECK(xt_gpu_target_resize(t, 32, 32) == XT_STATUS_INVALID_ARGUMENT);  // not inside a frame
    REQUIRE(xt_surface_render_gpu(state, t, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
    const Pixels small = readback(t, 64, 64, 64 * 4);
    CHECK(regionMatches(small, 64, 0, 0, swFrame(stateSurface(f), 64), 64, 64, "state after resize"));
}

TEST_CASE("capi gpu 3 (T-636): xt_surface_render and xt_surface_render_gpu are not mixed until detach") {
    Fixture f;
    xt_gpu_target* t = f.target(64, 64);
    if (t == nullptr) return;
    xt_surface* s = stateSurface(f);
    Pixels buf(64u * 64u * 4u, 0);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(xt_surface_render(s, buf.data(), 64 * 4, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
    CHECK(xt_surface_render_gpu(s, t, nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(contains(xt_capi_last_error(), "xt_surface_detach_gpu"));
    CHECK(xt_surface_detach_gpu(s) == XT_STATUS_OK);  // not attached: only resets the path
    CHECK(xt_surface_render_gpu(s, t, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
    CHECK(xt_surface_render(s, buf.data(), 64 * 4, nullptr) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_surface_detach_gpu(s) == XT_STATUS_OK);
    // After detach the SW path repaints the whole buffer (it matches a fresh surface's frame).
    std::fill(buf.begin(), buf.end(), uint8_t{0x5A});
    REQUIRE(xt_surface_render(s, buf.data(), 64 * 4, nullptr) == XT_STATUS_OK);
    CHECK(buf == swFrame(stateSurface(f), 64));
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);  // the detached surface holds nothing on it
    f.forget(t);
}

TEST_CASE("capi gpu 4 (T-636): a surface attached to one target cannot draw to a second") {
    Fixture f;
    xt_gpu_target* a = f.target(64, 64);
    if (a == nullptr) return;
    xt_gpu_target* b = f.target(64, 64);
    REQUIRE(b != nullptr);
    xt_surface* s = stateSurface(f);
    frame(a, {{s, xt_rect{0, 0, 64, 64}}});
    REQUIRE(xt_gpu_target_begin_frame(b, nullptr) == XT_STATUS_OK);
    CHECK(xt_surface_render_gpu(s, b, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(contains(xt_capi_last_error(), "attached to another target"));
    REQUIRE(xt_surface_detach_gpu(s) == XT_STATUS_OK);
    CHECK(xt_surface_render_gpu(s, b, nullptr) == XT_STATUS_OK);
    REQUIRE(xt_gpu_target_end_frame(b) == XT_STATUS_OK);
    CHECK(regionMatches(readback(b, 64, 64, 256), 64, 0, 0, swFrame(stateSurface(f), 64), 64, 64,
                        "state on b"));
    CHECK(xt_gpu_target_destroy(a) == XT_STATUS_OK);  // s left a
    f.forget(a);
}

TEST_CASE("capi gpu 5 (T-636): destroying a target with attached surfaces is BUSY") {
    Fixture f;
    xt_gpu_target* t = f.target(64, 64);
    if (t == nullptr) return;
    xt_surface* one = stateSurface(f);
    xt_surface* two = flipSurface(f, 16);
    frame(t, {{one, xt_rect{0, 0, 64, 64}}, {two, xt_rect{0, 0, 16, 16}}});
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_BUSY);
    CHECK(contains(xt_capi_last_error(), "2 surface(s) still attached"));
    frame(t, {{one, xt_rect{0, 0, 64, 64}}});  // still usable
    f.destroy(one);                            // destroying a surface detaches it
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_BUSY);
    CHECK(contains(xt_capi_last_error(), "1 surface(s) still attached"));
    REQUIRE(xt_surface_detach_gpu(two) == XT_STATUS_OK);
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    f.forget(t);
    CHECK(xt_gpu_target_destroy(nullptr) == XT_STATUS_OK);
}

TEST_CASE("capi gpu 6 (T-636): an unknown backend name, bad descriptors and calls out of order") {
    xt_gpu_target_desc desc = offscreenDesc(32, 32);
    desc.backend = "no-such-backend";
    CHECK(xt_gpu_target_create(&desc) == nullptr);
    CHECK(contains(xt_capi_last_error(), "no-such-backend"));
    CHECK(xt_gpu_target_create(nullptr) == nullptr);
    desc = offscreenDesc(32, 32);
    desc.struct_size = 4;
    CHECK(xt_gpu_target_create(&desc) == nullptr);
    CHECK(contains(xt_capi_last_error(), "struct_size"));
    desc = offscreenDesc(0, 32);
    CHECK(xt_gpu_target_create(&desc) == nullptr);
    desc = offscreenDesc(32, 32);
    desc.window.kind = 99;
    CHECK(xt_gpu_target_create(&desc) == nullptr);
    CHECK(contains(xt_capi_last_error(), "window kind"));
    desc = offscreenDesc(32, 32);
    desc.window.kind = XT_WINDOW_WIN32;  // a NULL HWND
    CHECK(xt_gpu_target_create(&desc) == nullptr);

    Fixture f;
    xt_gpu_target* t = f.target(32, 32);
    if (t == nullptr) return;
    xt_surface* s = flipSurface(f, 32);
    const xt_rect empty{0, 0, 0, 10};
    CHECK(xt_surface_render_gpu(s, t, nullptr) == XT_STATUS_INVALID_ARGUMENT);  // no frame begun
    CHECK(xt_gpu_target_end_frame(t) == XT_STATUS_INVALID_ARGUMENT);
    REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
    CHECK(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_render_gpu(s, nullptr, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_render_gpu(nullptr, t, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_render_gpu(s, t, &empty) == XT_STATUS_INVALID_ARGUMENT);
    const xt_rect outside{100, 100, 8, 8};
    CHECK(xt_surface_render_gpu(s, t, &outside) == XT_STATUS_OK);  // nothing visible, still OK
    REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
    Pixels buf(32u * 32u * 4u, 0);
    CHECK(xt_gpu_target_readback(t, nullptr, 128) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_gpu_target_readback(t, buf.data(), 64) == XT_STATUS_INVALID_ARGUMENT);
    REQUIRE(xt_gpu_target_readback(t, buf.data(), 128) == XT_STATUS_OK);
    CHECK(std::all_of(buf.begin(), buf.end(), [](uint8_t v) { return v == 0; }));
    CHECK(xt_gpu_target_resize(t, 0, 4) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_detach_gpu(nullptr) == XT_STATUS_INVALID_ARGUMENT);
}

namespace {

/// T-648: the target's shared-cache info lines (xt_capi_set_log), latest values; removed at scope end.
struct SharedLog {
    unsigned long textures = 0, uploaded = 0, reused = 0, freed = 0, sets = 0, borrowed = 0;
    unsigned long destroyedFreed = 0;
    int lines = 0;
    int released = 0;
    bool destroyed = false;
    SharedLog() { xt_capi_set_log(&SharedLog::onLog, this); }
    ~SharedLog() { xt_capi_set_log(nullptr, nullptr); }
    SharedLog(const SharedLog&) = delete;
    SharedLog& operator=(const SharedLog&) = delete;
    static unsigned long after(const char* message, const char* key) {
        const char* p = std::strstr(message, key);
        return p != nullptr ? std::strtoul(p + std::strlen(key), nullptr, 10) : 0ul;
    }
    static void onLog(int level, const char* message, void* user) {
        auto* self = static_cast<SharedLog*>(user);
        if (level != 0) return;
        if (std::strstr(message, "xt_gpu_target: shared image textures ") == message) {
            self->textures = after(message, "shared image textures ");
            self->uploaded = after(message, "uploaded ");
            self->reused = after(message, "reused ");
            self->freed = after(message, "freed ");
            self->sets = after(message, "pipeline sets ");
            self->borrowed = after(message, "borrowed ");
            ++self->lines;
        } else if (std::strstr(message, "xt_gpu_target: destroyed; ") == message) {
            self->destroyed = true;
            self->destroyedFreed = after(message, "destroyed; ");
        } else if (std::strstr(message, "GPU resources released") != nullptr) {
            ++self->released;
        }
    }
};

constexpr int kShareSize = 192;

/// An offscreen kShareSize target with `samples` (1 or 4), or null without an adapter.
xt_gpu_target* shareTarget(int samples) {
    xt_gpu_target_desc desc = offscreenDesc(kShareSize, kShareSize);
    desc.sample_count = samples;
    xt_gpu_target* t = xt_gpu_target_create(&desc);
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
        std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
    }
    return t;
}

/// One npdo bundle and two `settings` surfaces of it (both destroyed, then the bundle closed, at scope
/// end; the caller detaches them or destroys its target first).
struct SharedPair {
    xt_bundle* bundle = nullptr;
    xt_surface* s[2] = {nullptr, nullptr};
    SharedPair() {
        bundle = xt_bundle_open(XT_CAPI_TEST_NPDO_BUNDLE);
        REQUIRE_MESSAGE(bundle != nullptr, xt_capi_last_error());
        for (xt_surface*& surface : s) {
            xt_surface_desc desc{};
            desc.struct_size = sizeof(desc);
            desc.width = kShareSize;
            desc.height = kShareSize;
            desc.device_pixel_ratio = 1.0f;
            desc.premultiplied = 1;
            surface = xt_surface_create(bundle, "settings", &desc);
            REQUIRE_MESSAGE(surface != nullptr, xt_capi_last_error());
        }
    }
    ~SharedPair() {
        for (xt_surface* surface : s) xt_surface_destroy(surface);
        (void)xt_bundle_close(bundle);
    }
    SharedPair(const SharedPair&) = delete;
    SharedPair& operator=(const SharedPair&) = delete;
};

/// `s` alone over the whole target (update(0) first), read back.
Pixels shareFrame(xt_gpu_target* t, xt_surface* s) {
    frame(t, {{s, xt_rect{0, 0, kShareSize, kShareSize}}});
    return readback(t, kShareSize, kShareSize, kShareSize * 4);
}

}  // namespace

TEST_CASE("capi gpu 8 (T-648): two surfaces of one bundle on one target upload each image once") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        xt_gpu_target* t = shareTarget(samples);
        if (t == nullptr) return;
        {
            SharedPair p;
            const Pixels first = shareFrame(t, p.s[0]);
            REQUIRE(log.lines > 0);
            const unsigned long uploaded = log.uploaded;
            std::printf("samples %d, first surface: %lu image texture(s) uploaded, pipeline sets %lu\n",
                        samples, uploaded, log.sets);
            CHECK(uploaded > 0);  // npdo settings draws bundle images
            CHECK(log.reused == 0);
            CHECK(log.textures == uploaded);
            CHECK(log.sets == 1);      // made by the target's composer ...
            CHECK(log.borrowed == 1);  // ... and borrowed by the first surface's renderer
            const Pixels second = shareFrame(t, p.s[1]);
            CHECK(log.uploaded == uploaded);  // nothing uploaded again
            CHECK(log.reused == uploaded);    // every image of the second surface is the first one's
            CHECK(log.textures == uploaded);
            CHECK(log.sets == 1);
            CHECK(log.borrowed == 2);
            CHECK(first == second);  // the shared textures draw what the uploaded ones drew
            for (xt_surface* s : p.s) REQUIRE(xt_surface_detach_gpu(s) == XT_STATUS_OK);
        }
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
        CHECK(log.destroyed);
        CHECK(log.destroyedFreed == log.uploaded);
    }
}

TEST_CASE(
    "capi gpu 9 (T-648): a detach keeps the shared textures; the other surface draws on; destroy frees") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        xt_gpu_target* t = shareTarget(samples);
        if (t == nullptr) return;
        unsigned long uploaded = 0;
        {
            SharedPair p;
            (void)shareFrame(t, p.s[0]);
            const Pixels before = shareFrame(t, p.s[1]);
            uploaded = log.uploaded;
            REQUIRE(uploaded > 0);
            REQUIRE(xt_surface_detach_gpu(p.s[0]) == XT_STATUS_OK);
            CHECK(shareFrame(t, p.s[1]) == before);  // the other surface still draws its images
            // Both detached: the textures stay with the target, used by no surface ...
            REQUIRE(xt_surface_detach_gpu(p.s[1]) == XT_STATUS_OK);
            const unsigned long reused = log.reused;
            // ... and a surface attached again finds every one of them (no upload).
            CHECK(shareFrame(t, p.s[1]) == before);
            CHECK(log.uploaded == uploaded);
            CHECK(log.reused == reused + uploaded);
            CHECK(log.freed == 0);
            CHECK(log.textures == uploaded);
            REQUIRE(xt_surface_detach_gpu(p.s[1]) == XT_STATUS_OK);
        }
        CHECK(!log.destroyed);
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);  // frees them
        CHECK(log.destroyed);
        CHECK(log.destroyedFreed == uploaded);
    }
}

TEST_CASE("capi gpu 10 (T-648): a trimmed surface rebuilds from the shared textures and pipelines") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        xt_gpu_target* t = shareTarget(samples);
        if (t == nullptr) return;
        {
            SharedPair p;
            (void)shareFrame(t, p.s[1]);  // the other surface holds the textures too
            const Pixels before = shareFrame(t, p.s[0]);
            const unsigned long uploaded = log.uploaded, reused = log.reused, borrowed = log.borrowed;
            // T-641: hidden for two frames releases the surface's GPU resources (it stays attached).
            REQUIRE(xt_surface_set_visible(p.s[0], 0) == XT_STATUS_OK);
            (void)shareFrame(t, p.s[0]);
            (void)shareFrame(t, p.s[0]);
            CHECK(log.released == 1);
            REQUIRE(xt_surface_set_visible(p.s[0], 1) == XT_STATUS_OK);
            CHECK(shareFrame(t, p.s[0]) == before);  // rebuilt: same pixels
            CHECK(log.uploaded == uploaded);         // ... from the shared textures
            CHECK(log.reused == reused + uploaded);
            CHECK(log.borrowed == borrowed + 1);  // and the shared pipeline set
            CHECK(log.sets == 1);
            // xt_surface_trim on both: no texture is freed, the next frames upload nothing.
            REQUIRE(xt_surface_trim(p.s[0]) == XT_STATUS_OK);
            REQUIRE(xt_surface_trim(p.s[1]) == XT_STATUS_OK);
            CHECK(shareFrame(t, p.s[0]) == before);
            CHECK(log.uploaded == uploaded);
            CHECK(log.freed == 0);
            for (xt_surface* s : p.s) REQUIRE(xt_surface_detach_gpu(s) == XT_STATUS_OK);
        }
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
}

#ifdef _WIN32
TEST_CASE("capi gpu 7 (T-636): a Win32 window target presents and reads back inside a frame") {
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"xt_capi_tests gpu", WS_OVERLAPPEDWINDOW, 0, 0, 96, 96,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    REQUIRE(hwnd != nullptr);
    {
        Fixture f;
        xt_gpu_target_desc desc = offscreenDesc(64, 64);
        desc.window.kind = XT_WINDOW_WIN32;
        desc.window.u.win32.hwnd = hwnd;
        desc.vsync = 0;
        xt_gpu_target* t = xt_gpu_target_create(&desc);
        if (t == nullptr) {
            const std::string error = xt_capi_last_error();
            DestroyWindow(hwnd);
            REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
            std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
            return;
        }
        f.targets.push_back(t);
        xt_surface* s = flipSurface(f, 64);
        Pixels px(64u * 64u * 4u, 0);
        CHECK(xt_gpu_target_readback(t, px.data(), 256) == XT_STATUS_UNSUPPORTED);  // outside a frame
        for (int i = 0; i < 3; ++i) {
            REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
            REQUIRE_MESSAGE(xt_surface_render_gpu(s, t, nullptr) == XT_STATUS_OK, xt_capi_last_error());
            const xt_status read = xt_gpu_target_readback(t, px.data(), 256);
            if (read == XT_STATUS_OK) {
                CHECK(at(px, 64, 32, 32)[0] == 255);  // red, BGRA swapchains returned as RGBA
                CHECK(at(px, 64, 32, 32)[2] == 0);
            } else {
                CHECK(read == XT_STATUS_UNSUPPORTED);  // a swapchain without COPY_SRC
            }
            REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
        }
        CHECK(xt_gpu_target_resize(t, 80, 48) == XT_STATUS_OK);
        frame(t, {{s, xt_rect{0, 0, 80, 48}}});
    }
    DestroyWindow(hwnd);
}
#endif

#endif  // XT_CAPI_TEST_HAS_WGPU
