// T-636 (Docs/design/50-c-abi.md §50.z): the GPU target of the C ABI -- xt_gpu_target_* and
// xt_surface_render_gpu / xt_surface_detach_gpu -- on an offscreen (XT_WINDOW_NONE) target read back
// with xt_gpu_target_readback. Each composited region is compared with the same UI rendered by
// xt_surface_render (premultiplied) at 15 §15.3's GPU tolerance (channel 16, 3 % of the pixels; as
// Tests/wgpu). Fixtures: Tests/capi/fixtures/state (64x64, four rows) and fixtures/flip (a red card).
// XT_CAPI_TEST_HAS_WGPU is set when the library was built with xt_wgpu; without it only the
// "built without wgpu" answers are checked. With wgpu but no adapter the cases print SKIP and pass.
// T-648 (cases 8-10, at sample counts 1 and 4): the target's shared image textures and pipeline set,
// observed through its info log lines (npdo `settings`, two surfaces of one bundle).
// T-656 (cases 9-13, 50 §50.ab item 1): the idle budget of those textures, xt_gpu_target_trim and
// xt_gpu_target_memory_stats; cases 11-13 and 15 use fixtures/images (three 1024 x 1024 images), case 11
// rewriting its images with new pixels every round in a copy under the test temp root.
// T-658 (case 14, Docs/design/50-c-abi.md §50.ab item 3): a surface whose frame has no draw command
// (fixtures/flip `ghost`, its card hidden by a port) is neither drawn nor composited and gets no
// texture; its info lines carry the target's submit / pass totals.
// T-674 (cases 16-17, 50 §50.ac item 1): at most two frames in flight per target, observed through the
// frames-in-flight counters of the target's destroy line.
// T-675 (cases 18-20, 50 §50.ac item 2): the target-level pools -- a scene switch creates no texture once
// warm, surfaces of one size share one attachment set, the pools age and xt_gpu_target_trim empties them;
// read through the textures-created and pool fields of the shared-cache info line. Cases 10, 12 and 15
// count trim-on-hide's "GPU textures returned" line where they counted "GPU resources released".
// T-678 (cases 21-22, 50 §50.ac addendum item 1): the transient pool's budget is the last target frame's
// borrowed peak plus 16 MiB -- it shrinks after a switch to a lighter scene and never churns a steady one.
// T-683 (cases 23-24, 50 §50.ad items 2-3): the target prewarms every built-in shader at creation, so the
// first frame of a surface with analytic box shadows compiles none (the shared-cache line's "after
// prewarm" counters); a hidden surface releases its coverage atlas (atlas_bytes falls) and draws the same
// bytes when shown again. Fixture: fixtures/boxshadow.
// T-685 (cases 26-27, 50 §50.ac addendum 2): a transient whose last borrower is a visible surface is not
// evicted by the budget, only by the 1800-frame ageing -- a visible surface alternating between a heavy and
// a light state (fixtures/effects, its `heavy` port) rebuilds nothing from its second period on, hidden it
// gives its transients back to the budget; read through the shared-cache line's "transient pool protected",
// "budget evictions" and "age evictions".
// T-686 (case 28, 10 §10.13 ruling 1): fixtures/images with its three images replaced by BC7 DDS content
// (full chains): image_bytes counts the blocks and the stored levels, and the idle budget applies as to
// RGBA images.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "../common/test_temp.h"
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
    // T-666: the cases written when 0 meant 4 keep their 4x target by asking for it; the cases that
    // loop over 1 and 4 set it themselves, and capi gpu 25 checks what 0 gives now.
    desc.sample_count = 4;
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
    // T-656
    CHECK(xt_gpu_target_set_image_budget(nullptr, 0) == XT_STATUS_UNSUPPORTED);
    CHECK(xt_gpu_target_trim(nullptr) == XT_STATUS_UNSUPPORTED);
    xt_gpu_memory_stats stats{};
    stats.struct_size = sizeof stats;
    CHECK(xt_gpu_target_memory_stats(nullptr, &stats) == XT_STATUS_UNSUPPORTED);
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
    /// T-675: every texture created on the target's device so far, and the target-level pools.
    unsigned long created = 0, poolTextures = 0, attachmentSets = 0, idleSurfaceTextures = 0, poolFrees = 0;
    /// T-683: the prewarm line of the target's creation, and the shared-cache line's "after prewarm"
    /// counters.
    unsigned long prewarmedShaders = 0, prewarmedPipelines = 0, shaderCompiles = 0, pipelineCreates = 0;
    /// T-685: the transient bytes (KiB) the budget may not evict, and the budget / ageing evictions so far.
    unsigned long protectedKiB = 0, budgetEvictions = 0, ageEvictions = 0;
    int prewarms = 0;
    int lines = 0;
    int released = 0;
    int returned = 0;  ///< T-675: trim-on-hide gave the textures back (the renderer stays)
    int rebuilt = 0;   ///< T-675: a released surface built its renderer again
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
            self->created = after(message, "textures created ");
            self->poolTextures = after(message, "; pools ");
            self->attachmentSets = after(message, "attachment sets ");
            self->idleSurfaceTextures = after(message, "idle surface textures ");
            self->poolFrees = after(message, "pool frees ");
            self->shaderCompiles = after(message, "after prewarm: shader compiles ");
            self->pipelineCreates = after(message, "pipeline creates ");
            self->protectedKiB = after(message, "transient pool protected ");
            self->budgetEvictions = after(message, "budget evictions ");
            self->ageEvictions = after(message, "age evictions ");
            ++self->lines;
        } else if (std::strstr(message, "xt_gpu_target: prewarmed ") == message) {
            self->prewarmedShaders = after(message, "prewarmed ");
            self->prewarmedPipelines = after(message, "shader(s), ");
            ++self->prewarms;
        } else if (std::strstr(message, "xt_gpu_target: destroyed; ") == message) {
            self->destroyed = true;
            self->destroyedFreed = after(message, "destroyed; ");
        } else if (std::strstr(message, "GPU resources released") != nullptr) {
            ++self->released;
        } else if (std::strstr(message, "GPU textures returned to the target") != nullptr) {
            ++self->returned;
        } else if (std::strstr(message, "GPU resources rebuilt") != nullptr) {
            ++self->rebuilt;
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

/// T-656: xt_gpu_target_memory_stats of `t` (the whole struct).
xt_gpu_memory_stats memoryStats(const xt_gpu_target* t) {
    xt_gpu_memory_stats m;
    std::memset(&m, 0, sizeof m);
    m.struct_size = sizeof m;
    REQUIRE_MESSAGE(xt_gpu_target_memory_stats(t, &m) == XT_STATUS_OK, xt_capi_last_error());
    return m;
}

constexpr uint64_t kMiB = uint64_t(1) << 20;
constexpr uint64_t kDefaultIdleBudget = 64 * kMiB;  // 50 §50.ab item 1

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
            // T-656: unused now, idle within the default budget (npdo settings: well under 64 MiB).
            const xt_gpu_memory_stats m = memoryStats(t);
            CHECK(m.image_textures == uploaded);
            CHECK(m.image_idle_textures == uploaded);
            CHECK(m.image_idle_bytes == m.image_bytes);
            CHECK(m.image_idle_bytes <= kDefaultIdleBudget);
            CHECK(m.image_evictions == 0);
            CHECK(m.attached_surfaces == 0);
        }
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
        CHECK(log.destroyed);
        CHECK(log.destroyedFreed == log.uploaded);
    }
}

TEST_CASE(
    "capi gpu 9 (T-648, T-656): a detach leaves the shared textures idle within the budget; budget 0 frees") {
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
            // Both detached: the textures stay with the target, used by no surface (idle, within the
            // default 64 MiB budget) ...
            REQUIRE(xt_surface_detach_gpu(p.s[1]) == XT_STATUS_OK);
            xt_gpu_memory_stats m = memoryStats(t);
            CHECK(m.image_idle_textures == uploaded);
            CHECK(m.image_idle_bytes <= kDefaultIdleBudget);
            const unsigned long reused = log.reused;
            // ... and a surface attached again finds every one of them (no upload).
            CHECK(shareFrame(t, p.s[1]) == before);
            CHECK(log.uploaded == uploaded);
            CHECK(log.reused == reused + uploaded);
            CHECK(log.freed == 0);
            CHECK(log.textures == uploaded);
            m = memoryStats(t);
            CHECK(m.image_idle_textures == 0);  // in use again
            CHECK(m.image_textures == uploaded);
            // Budget 0: a texture is destroyed as soon as no surface uses it ...
            REQUIRE(xt_gpu_target_set_image_budget(t, 0) == XT_STATUS_OK);
            REQUIRE(xt_surface_detach_gpu(p.s[1]) == XT_STATUS_OK);
            m = memoryStats(t);
            CHECK(m.image_textures == 0);
            CHECK(m.image_bytes == 0);
            CHECK(m.image_idle_textures == 0);
            CHECK(m.image_evictions == uploaded);
            // ... so attaching again uploads again, and draws the same.
            CHECK(shareFrame(t, p.s[1]) == before);
            CHECK(log.uploaded == 2 * uploaded);
            REQUIRE(xt_surface_detach_gpu(p.s[1]) == XT_STATUS_OK);
            CHECK(memoryStats(t).image_textures == 0);
        }
        CHECK(!log.destroyed);
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
        CHECK(log.destroyed);
        CHECK(log.destroyedFreed == 0);  // nothing was left
    }
}

TEST_CASE("capi gpu 10 (T-648, T-656): a trimmed surface rebuilds from the shared textures and pipelines") {
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
            // T-641, T-675 (50 §50.ac item 2): hidden for two frames gives the surface's textures back to
            // the target and ends its image uses; its renderer stays (it stays attached).
            REQUIRE(xt_surface_set_visible(p.s[0], 0) == XT_STATUS_OK);
            (void)shareFrame(t, p.s[0]);
            (void)shareFrame(t, p.s[0]);
            CHECK(log.returned == 1);
            CHECK(log.released == 0);
            REQUIRE(xt_surface_set_visible(p.s[0], 1) == XT_STATUS_OK);
            CHECK(shareFrame(t, p.s[0]) == before);  // same pixels
            CHECK(log.rebuilt == 0);                 // the kept renderer
            CHECK(log.uploaded == uploaded);         // ... draws from the shared textures
            CHECK(log.reused == reused + uploaded);
            CHECK(log.borrowed == borrowed);  // no new renderer borrowed the pipeline set
            CHECK(log.sets == 1);
            // xt_surface_trim on both releases everything: the textures go idle (within the default
            // budget), none is freed, the next frames upload nothing; the renderer is built again and
            // borrows the shared pipeline set.
            REQUIRE(xt_surface_trim(p.s[0]) == XT_STATUS_OK);
            REQUIRE(xt_surface_trim(p.s[1]) == XT_STATUS_OK);
            CHECK(log.released == 2);
            CHECK(memoryStats(t).image_idle_textures == uploaded);
            CHECK(shareFrame(t, p.s[0]) == before);
            CHECK(log.rebuilt == 1);
            CHECK(log.borrowed == borrowed + 1);
            CHECK(log.uploaded == uploaded);
            CHECK(log.freed == 0);
            for (xt_surface* s : p.s) REQUIRE(xt_surface_detach_gpu(s) == XT_STATUS_OK);
        }
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
}

namespace {

constexpr int kImagesSize = 256;  // fixtures/images: the UI's size
constexpr int kImageSide = 1024;  // ... and each of its three images'
/// T-665 (Docs/design/10-backends.md §10.11 item 7, 50 §50.ab): the UI draws each image minified, so
/// its texture carries a mip chain: levels 1..10 of a 1024 x 1024 RGBA8 image, (4^10 - 1) / 3 texels.
constexpr uint64_t kImageMipBytes = (uint64_t(1) << 20) / 3u * 4u;
/// One image texture as the target counts it: level 0 and its chain (about 4/3 of level 0).
constexpr uint64_t kImageBytes = uint64_t(kImageSide) * kImageSide * 4u + kImageMipBytes;
static_assert(kImageMipBytes == 1398100u, "levels 1..10 of 1024 x 1024 RGBA8");

/// A PNG of `side` x `side` RGBA8 pixels, stored (uncompressed deflate), whose content depends on
/// `seed` (every seed gives other pixels).
std::vector<uint8_t> makePng(int side, uint32_t seed) {
    static uint32_t crcTable[256];
    if (crcTable[1] == 0) {
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crcTable[n] = c;
        }
    }
    std::vector<uint8_t> raw;  // filter byte 0 + one row
    const size_t row = static_cast<size_t>(side) * 4u + 1u;
    raw.reserve(row * static_cast<size_t>(side));
    for (int y = 0; y < side; ++y) {
        raw.push_back(0);
        for (int x = 0; x < side; ++x) {
            raw.push_back(static_cast<uint8_t>(x + static_cast<int>(seed) * 37));
            raw.push_back(static_cast<uint8_t>(y ^ static_cast<int>(seed)));
            raw.push_back(static_cast<uint8_t>(seed * 7u));
            raw.push_back(255);
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(i),
                 raw.begin() + static_cast<std::ptrdiff_t>(i + n));
    }
    for (const uint8_t v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    const uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k) z.push_back(static_cast<uint8_t>(adler >> (8 * k)));
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto be32 = [&png](uint32_t v) {
        for (int k = 3; k >= 0; --k) png.push_back(static_cast<uint8_t>(v >> (8 * k)));
    };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(static_cast<uint32_t>(data.size()));
        const size_t from = png.size();
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        uint32_t c = 0xFFFFFFFFu;
        for (size_t i = from; i < png.size(); ++i) c = crcTable[(c ^ png[i]) & 0xFFu] ^ (c >> 8);
        be32(c ^ 0xFFFFFFFFu);
    };
    std::vector<uint8_t> ihdr;
    for (int k = 3; k >= 0; --k) ihdr.push_back(static_cast<uint8_t>(static_cast<uint32_t>(side) >> (8 * k)));
    for (int k = 3; k >= 0; --k) ihdr.push_back(static_cast<uint8_t>(static_cast<uint32_t>(side) >> (8 * k)));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8 bit RGBA, no interlace
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

/// A `kImagesSize` surface of the `images` UI of the bundle at `dir` (the bundle stays open with it).
struct ImagesSurface {
    xt_bundle* bundle = nullptr;
    xt_surface* s = nullptr;
    explicit ImagesSurface(const std::string& dir) {
        bundle = xt_bundle_open(dir.c_str());
        REQUIRE_MESSAGE(bundle != nullptr, xt_capi_last_error());
        xt_surface_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.width = kImagesSize;
        desc.height = kImagesSize;
        desc.device_pixel_ratio = 1.0f;
        desc.premultiplied = 1;
        s = xt_surface_create(bundle, "images", &desc);
        REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    }
    ~ImagesSurface() {
        xt_surface_destroy(s);
        (void)xt_bundle_close(bundle);
    }
    ImagesSurface(const ImagesSurface&) = delete;
    ImagesSurface& operator=(const ImagesSurface&) = delete;
    void frames(xt_gpu_target* t, int n) {
        for (int i = 0; i < n; ++i) frame(t, {{s, xt_rect{0, 0, kImagesSize, kImagesSize}}});
    }
};

}  // namespace

namespace {

/// T-658: the `ghost` surface's "has nothing to draw" / "draws again" info lines, with the target's
/// submit and pass totals they carry (latest values); removed at scope end.
struct EmptyLog {
    int empty = 0, again = 0;
    bool noTexture = false;
    unsigned long emptySubmits = 0, emptyPasses = 0, againSubmits = 0, againPasses = 0;
    EmptyLog() { xt_capi_set_log(&EmptyLog::onLog, this); }
    ~EmptyLog() { xt_capi_set_log(nullptr, nullptr); }
    EmptyLog(const EmptyLog&) = delete;
    EmptyLog& operator=(const EmptyLog&) = delete;
    static void onLog(int level, const char* message, void* user) {
        auto* self = static_cast<EmptyLog*>(user);
        if (level != 0 || std::strstr(message, "xt_surface_render_gpu: ghost ") != message) return;
        if (std::strstr(message, " has nothing to draw") != nullptr) {
            ++self->empty;
            self->noTexture = std::strstr(message, "no texture") != nullptr;
            self->emptySubmits = SharedLog::after(message, "target ");
            self->emptyPasses = SharedLog::after(message, " submits, ");
        } else if (std::strstr(message, " draws again") != nullptr) {
            ++self->again;
            self->againSubmits = SharedLog::after(message, "target ");
            self->againPasses = SharedLog::after(message, " submits, ");
        }
    }
};

}  // namespace

TEST_CASE("capi gpu 11 (T-656): new images every round on one target stay within the idle budget") {
    namespace fs = std::filesystem;
    for (const bool zeroBudget : {true, false}) {
        CAPTURE(zeroBudget);
        xt_gpu_target* t = makeTarget(kImagesSize, kImagesSize);
        if (t == nullptr) return;
        if (zeroBudget) REQUIRE(xt_gpu_target_set_image_budget(t, 0) == XT_STATUS_OK);
        const fs::path dir = xt::test::tempDir() / (zeroBudget ? "t656-images-0" : "t656-images-default");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::copy(fs::path(XT_CAPI_TEST_IMAGES_BUNDLE), dir, fs::copy_options::recursive, ec);
        REQUIRE_MESSAGE(!ec, ec.message());
        std::vector<fs::path> pngs;
        for (const fs::directory_entry& e : fs::directory_iterator(dir / "assets")) {
            if (e.path().extension() == ".png") pngs.push_back(e.path());
        }
        std::sort(pngs.begin(), pngs.end());
        REQUIRE(pngs.size() == 3);
        const uint64_t perImage = kImageBytes;  // T-665: with its mip chain
        std::vector<uint64_t> bytesAfter;
        for (uint32_t round = 0; round < 8; ++round) {
            CAPTURE(round);
            for (size_t i = 0; i < pngs.size(); ++i) {  // new pixels: new textures
                const std::vector<uint8_t> png =
                    makePng(kImageSide, round * 3u + static_cast<uint32_t>(i) + 1u);
                std::ofstream out(pngs[i], std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(png.data()),
                          static_cast<std::streamsize>(png.size()));
                REQUIRE(out.good());
            }
            {
                ImagesSurface u(dir.string());
                u.frames(t, 3);
                const xt_gpu_memory_stats m = memoryStats(t);
                CHECK(m.image_textures - m.image_idle_textures == 3);  // this round's three, in use
                CHECK(m.attached_surfaces == 1);
            }  // destroyed: its textures are no longer used
            const xt_gpu_memory_stats m = memoryStats(t);
            CHECK(m.attached_surfaces == 0);
            CHECK(m.image_bytes == m.image_idle_bytes);
            if (zeroBudget) {
                CHECK(m.image_bytes == 0);
                CHECK(m.image_textures == 0);
                CHECK(m.image_evictions == 3u * (round + 1u));  // each evicted as soon as unused
            } else {
                CHECK(m.image_idle_bytes <= kDefaultIdleBudget);
                CHECK(m.image_idle_bytes == uint64_t(m.image_idle_textures) * perImage);
            }
            bytesAfter.push_back(m.image_bytes);
        }
        std::printf("budget %s: image bytes after each round (MiB):", zeroBudget ? "0" : "default");
        for (const uint64_t b : bytesAfter) std::printf(" %.0f", static_cast<double>(b) / kMiB);
        std::printf("\n");
        if (!zeroBudget) {
            // 12 MiB a round: the budget is reached in round 6, after which the bytes stay flat.
            CHECK(bytesAfter[5] == bytesAfter[6]);
            CHECK(bytesAfter[6] == bytesAfter[7]);
            CHECK(bytesAfter[7] <= kDefaultIdleBudget);
            CHECK(memoryStats(t).image_evictions > 0);
        }
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
}

TEST_CASE("capi gpu 12 (T-656): hidden then shown within the budget uploads nothing again") {
    SharedLog log;
    xt_gpu_target* t = makeTarget(kImagesSize, kImagesSize);
    if (t == nullptr) return;
    {
        ImagesSurface u(XT_CAPI_TEST_IMAGES_BUNDLE);
        u.frames(t, 2);
        const unsigned long uploaded = log.uploaded;
        CHECK(uploaded == 3);
        // T-641, T-675: hidden for two frames gives the surface texture back to the target's pool and
        // ends the image uses (the renderer stays; it stays attached).
        REQUIRE(xt_surface_set_visible(u.s, 0) == XT_STATUS_OK);
        u.frames(t, 2);
        CHECK(log.returned == 1);
        xt_gpu_memory_stats m = memoryStats(t);
        CHECK(m.attached_surfaces == 1);
        CHECK(m.image_textures == 3);
        CHECK(m.image_idle_textures == 3);
        CHECK(m.surface_bytes == 0);  // returned (counted in transient_bytes while idle in the pool)
        REQUIRE(xt_surface_set_visible(u.s, 1) == XT_STATUS_OK);
        u.frames(t, 1);
        CHECK(log.uploaded == uploaded);  // found on the idle list
        m = memoryStats(t);
        CHECK(m.image_textures == 3);
        CHECK(m.image_idle_textures == 0);
        CHECK(m.image_evictions == 0);
        REQUIRE(xt_surface_detach_gpu(u.s) == XT_STATUS_OK);
    }
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
}

TEST_CASE("capi gpu 13 (T-656): xt_gpu_target_trim, the budget applied at once and the memory stats") {
    SharedLog log;
    xt_gpu_target* t = makeTarget(kImagesSize, kImagesSize);
    if (t == nullptr) return;
    CHECK(xt_gpu_target_set_image_budget(nullptr, 0) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_gpu_target_trim(nullptr) == XT_STATUS_INVALID_ARGUMENT);
    xt_gpu_memory_stats bad{};
    bad.struct_size = sizeof bad;
    CHECK(xt_gpu_target_memory_stats(nullptr, &bad) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_gpu_target_memory_stats(t, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    const uint64_t perImage = kImageBytes;                                  // T-665: with its mip chain
    CHECK(perImage * 3 == uint64_t(kImageSide) * kImageSide * 4u * 4 - 4);  // 4/3 of level 0, to the byte
    {
        ImagesSurface u(XT_CAPI_TEST_IMAGES_BUNDLE);
        u.frames(t, 2);
        xt_gpu_memory_stats m = memoryStats(t);
        CHECK(m.attached_surfaces == 1);
        CHECK(m.image_textures == 3);
        CHECK(m.image_bytes == 3 * perImage);
        // The surface texture (RGBA8) at least, and at 4x its MSAA color and stencil.
        const uint64_t texture = uint64_t(kImagesSize) * kImagesSize * 4u;
        CHECK(m.surface_bytes >= texture);
        CHECK(m.surface_bytes <= texture * 6u);
        // A shorter struct gets only its prefix.
        xt_gpu_memory_stats prefix;
        std::memset(&prefix, 0xAB, sizeof prefix);
        prefix.struct_size = static_cast<uint32_t>(offsetof(xt_gpu_memory_stats, image_idle_textures));
        REQUIRE(xt_gpu_target_memory_stats(t, &prefix) == XT_STATUS_OK);
        CHECK(prefix.image_textures == 3);
        CHECK(prefix.image_bytes == 3 * perImage);
        CHECK(prefix.image_idle_textures == 0xABABABABu);
        CHECK(prefix.atlas_bytes == 0xABABABABABABABABull);
        // Inside a frame: refused, nothing destroyed.
        REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
        CHECK(xt_gpu_target_trim(t) == XT_STATUS_INVALID_ARGUMENT);
        CHECK(contains(xt_capi_last_error(), "inside a frame"));
        REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
        REQUIRE(xt_surface_detach_gpu(u.s) == XT_STATUS_OK);
        m = memoryStats(t);
        CHECK(m.attached_surfaces == 0);
        CHECK(m.surface_bytes == 0);
        CHECK(m.image_idle_textures == 3);
        CHECK(m.image_idle_bytes == 3 * perImage);
        // A smaller budget applies at once: the oldest go until the rest fit (one texture with its chain).
        REQUIRE(xt_gpu_target_set_image_budget(t, perImage + perImage / 2) == XT_STATUS_OK);
        m = memoryStats(t);
        CHECK(m.image_idle_textures == 1);
        CHECK(m.image_idle_bytes == perImage);
        CHECK(m.image_evictions == 2);
        // Trim: every idle texture goes.
        REQUIRE(xt_gpu_target_trim(t) == XT_STATUS_OK);
        m = memoryStats(t);
        CHECK(m.image_textures == 0);
        CHECK(m.image_bytes == 0);
        CHECK(m.image_idle_textures == 0);
        CHECK(m.image_idle_bytes == 0);
        CHECK(m.image_evictions == 3);
        CHECK(xt_gpu_target_trim(t) == XT_STATUS_OK);  // nothing left: still OK
        // Drawn again: uploaded again.
        const unsigned long uploaded = log.uploaded;
        u.frames(t, 1);
        CHECK(log.uploaded == uploaded + 3);
        CHECK(memoryStats(t).image_textures == 3);
        REQUIRE(xt_surface_detach_gpu(u.s) == XT_STATUS_OK);
    }
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
}

namespace {

/// T-686: a DX10 BC7 DDS of `side` x `side` with its full mip chain; the block bytes depend on
/// `seed` (their decoded look does not matter here, only that each image is other content).
std::vector<uint8_t> makeBc7Dds(int side, uint32_t seed) {
    int levels = 1;
    size_t blocks = 0;
    for (int s = side;; s = std::max(1, s / 2), ++levels) {
        blocks += static_cast<size_t>((s + 3) / 4) * static_cast<size_t>((s + 3) / 4) * 16u;
        if (s == 1) break;
    }
    std::vector<uint8_t> dds(148 + blocks, 0);
    auto le32 = [&dds](size_t at, uint32_t v) {
        for (int k = 0; k < 4; ++k) dds[at + static_cast<size_t>(k)] = static_cast<uint8_t>(v >> (8 * k));
    };
    dds[0] = 'D', dds[1] = 'D', dds[2] = 'S', dds[3] = ' ';
    le32(4, 124);
    le32(8, 0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u | 0x80000u);  // ... MIPMAPCOUNT, LINEARSIZE
    le32(12, static_cast<uint32_t>(side));
    le32(16, static_cast<uint32_t>(side));
    le32(28, static_cast<uint32_t>(levels));
    le32(76, 32);
    le32(80, 0x4u);                                          // DDPF_FOURCC
    le32(84, 'D' | ('X' << 8) | ('1' << 16) | ('0' << 24));  // "DX10"
    le32(108, 0x1000u | 0x400000u | 0x8u);                   // TEXTURE | MIPMAP | COMPLEX
    le32(128, 98);                                           // DXGI_FORMAT_BC7_UNORM
    le32(132, 3);                                            // TEXTURE2D
    le32(140, 1);
    uint32_t x = seed * 2654435761u + 1u;
    for (size_t i = 148; i < dds.size(); i += 16) {
        dds[i] = 0x40;  // mode 6: every block is a valid BC7 block
        for (size_t k = 1; k < 16; ++k) {
            x = x * 1664525u + 1013904223u;
            dds[i + k] = static_cast<uint8_t>(x >> 24);
        }
    }
    return dds;
}

}  // namespace

TEST_CASE("capi gpu 28 (T-686): BC7 images count their blocks with the stored levels, idle budget included") {
    namespace fs = std::filesystem;
    SharedLog log;
    xt_gpu_target* t = makeTarget(kImagesSize, kImagesSize);
    if (t == nullptr) return;
    const fs::path dir = xt::test::tempDir() / "t686-images-bc7";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::copy(fs::path(XT_CAPI_TEST_IMAGES_BUNDLE), dir, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, ec.message());
    // The three images become BC7 DDS content under their names (the resolver tells a DDS by its bytes).
    std::vector<fs::path> images;
    for (const fs::directory_entry& e : fs::directory_iterator(dir / "assets")) {
        if (e.path().extension() == ".png") images.push_back(e.path());
    }
    std::sort(images.begin(), images.end());
    REQUIRE(images.size() == 3);
    for (size_t i = 0; i < images.size(); ++i) {
        const std::vector<uint8_t> dds = makeBc7Dds(kImageSide, static_cast<uint32_t>(i) + 1u);
        std::ofstream out(images[i], std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
        REQUIRE(out.good());
    }
    // Level 0 (256 x 256 blocks of 16 bytes) and the stored levels 1..10 at their block sizes.
    uint64_t perImage = 0;
    for (int s = kImageSide;; s = std::max(1, s / 2)) {
        perImage += uint64_t((s + 3) / 4) * uint64_t((s + 3) / 4) * 16u;
        if (s == 1) break;
    }
    CHECK(perImage == 1048576u + 349552u);
    {
        ImagesSurface u(dir.string());
        u.frames(t, 2);
        CHECK(log.uploaded == 3);
        xt_gpu_memory_stats m = memoryStats(t);
        CHECK(m.image_textures == 3);
        CHECK(m.image_bytes == 3 * perImage);  // blocks, with the levels the minified draws asked for
        REQUIRE(xt_surface_detach_gpu(u.s) == XT_STATUS_OK);
        m = memoryStats(t);
        CHECK(m.image_idle_textures == 3);
        CHECK(m.image_idle_bytes == 3 * perImage);
        REQUIRE(xt_gpu_target_set_image_budget(t, perImage + perImage / 2) == XT_STATUS_OK);
        m = memoryStats(t);
        CHECK(m.image_idle_textures == 1);
        CHECK(m.image_idle_bytes == perImage);
        CHECK(m.image_evictions == 2);
        REQUIRE(xt_gpu_target_trim(t) == XT_STATUS_OK);
        CHECK(memoryStats(t).image_bytes == 0);
    }
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    fs::remove_all(dir, ec);
}

TEST_CASE("capi gpu 14 (T-658): a surface with nothing to draw is not drawn, composited or given a texture") {
    constexpr int kSize = 96;
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        EmptyLog log;
        Fixture f;
        xt_gpu_target_desc desc = offscreenDesc(kSize, kSize);
        desc.sample_count = samples;
        xt_gpu_target* t = xt_gpu_target_create(&desc);
        if (t == nullptr) {
            const std::string error = xt_capi_last_error();
            REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
            std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
            return;
        }
        f.targets.push_back(t);
        // The `ghost` fixture: its only card is hidden until the `show` port is true.
        xt_surface* ghost = f.surface(XT_CAPI_TEST_FLIP_BUNDLE, "ghost", 64);
        const xt_rect gr{16, 16, 64, 64};
        const Pixels none(static_cast<size_t>(kSize) * kSize * 4u, 0);
        // Four frames of the empty surface alone: the frame's clear is the only work, no texture.
        for (int i = 0; i < 4; ++i) {
            frame(t, {{ghost, gr}});
            CHECK(readback(t, kSize, kSize, kSize * 4) == none);
        }
        CHECK(log.empty == 1);  // logged once, when it became empty
        CHECK(log.noTexture);
        CHECK(memoryStats(t).surface_bytes == 0u);  // no texture, no 4x attachments
        xt_frame_stats stats{};
        REQUIRE(xt_surface_frame_stats(ghost, &stats) == XT_STATUS_OK);
        CHECK(stats.executed_draws == 0u);
        // The content appears: the next frame draws it.
        const uint32_t show = xt_surface_port_lookup(ghost, "show");
        REQUIRE(xt_surface_port_set_bool(ghost, show, 1) == XT_STATUS_OK);
        frame(t, {{ghost, gr}});
        REQUIRE(log.again == 1);
        // Between the two lines: one submit and one pass per frame (end_frame's clear), nothing of the
        // empty surface (no renderer submit, no composite).
        CHECK(log.againSubmits - log.emptySubmits == 4u);
        CHECK(log.againPasses - log.emptyPasses == 4u);
        const Pixels shown = readback(t, kSize, kSize, kSize * 4);
        CHECK(at(shown, kSize, 48, 48)[1] == 255);  // the green card, premultiplied opaque
        CHECK(at(shown, kSize, 48, 48)[3] == 255);
        CHECK(transparent(shown, kSize, 4, 4));
        REQUIRE(xt_surface_frame_stats(ghost, &stats) == XT_STATUS_OK);
        CHECK(stats.executed_draws > 0u);
        CHECK(memoryStats(t).surface_bytes > 0u);

        // Hidden again over another surface: the target reads back as if it had not been drawn.
        xt_surface* flip = flipSurface(f, 32);
        frame(t, {{flip, xt_rect{0, 0, 96, 96}}});
        const Pixels alone = readback(t, kSize, kSize, kSize * 4);
        REQUIRE(xt_surface_port_set_bool(ghost, show, 0) == XT_STATUS_OK);
        frame(t, {{flip, xt_rect{0, 0, 96, 96}}, {ghost, gr}});
        CHECK(readback(t, kSize, kSize, kSize * 4) == alone);
        CHECK(log.empty == 2);
        CHECK(!log.noTexture);  // the texture made while it drew is kept
        frame(t, {{flip, xt_rect{0, 0, 96, 96}}, {ghost, gr}});
        CHECK(readback(t, kSize, kSize, kSize * 4) == alone);
        CHECK(log.empty == 2);
    }
}

// T-657 (50 §50.ab item 2): hiding also lets the surface's Document go of its decoded images; shown
// again, the rebuilt renderer finds the shared textures by the keys the bundle's resolver kept (no
// decode -- counted in Tests/gpu/test_gpu_retained_key.cpp, the C ABI has no decode count) and draws
// the same bytes. With the target's image budget 0 the textures are gone and everything is uploaded
// again, still the same bytes.
TEST_CASE("capi gpu 15 (T-657): hidden then shown draws the same bytes from the shared textures") {
    SharedLog log;
    xt_gpu_target* t = makeTarget(kImagesSize, kImagesSize);
    if (t == nullptr) return;
    {
        ImagesSurface u(XT_CAPI_TEST_IMAGES_BUNDLE);
        u.frames(t, 2);
        const Pixels before = readback(t, kImagesSize, kImagesSize, kImagesSize * 4);
        REQUIRE(log.uploaded == 3);
        REQUIRE(xt_surface_set_visible(u.s, 0) == XT_STATUS_OK);
        u.frames(t, 2);
        CHECK(log.returned == 1);  // T-675: the textures go back, the renderer stays
        CHECK(memoryStats(t).image_idle_textures == 3);
        REQUIRE(xt_surface_set_visible(u.s, 1) == XT_STATUS_OK);
        u.frames(t, 1);
        CHECK(readback(t, kImagesSize, kImagesSize, kImagesSize * 4) == before);
        CHECK(log.uploaded == 3);  // nothing uploaded again
        CHECK(memoryStats(t).image_idle_textures == 0);
        // Budget 0: hidden destroys the textures; shown uploads all three again.
        REQUIRE(xt_gpu_target_set_image_budget(t, 0) == XT_STATUS_OK);
        REQUIRE(xt_surface_set_visible(u.s, 0) == XT_STATUS_OK);
        u.frames(t, 2);
        CHECK(log.returned == 2);
        CHECK(memoryStats(t).image_textures == 0);
        REQUIRE(xt_surface_set_visible(u.s, 1) == XT_STATUS_OK);
        u.frames(t, 1);
        CHECK(readback(t, kImagesSize, kImagesSize, kImagesSize * 4) == before);
        CHECK(log.uploaded == 6);
        REQUIRE(xt_surface_detach_gpu(u.s) == XT_STATUS_OK);
    }
    CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
}

namespace {

/// T-674: the target's destroy line ("...; frames in flight: W wait(s), X ms, peak P; S submits, Q passes")
/// and the begin_frame warnings (xt_capi_set_log; removed at scope end).
struct InflightLog {
    bool destroyed = false;
    unsigned long waits = 0, peak = 0, submits = 0, passes = 0;
    double waitMs = -1;
    int warnings = 0;
    InflightLog() { xt_capi_set_log(&InflightLog::onLog, this); }
    ~InflightLog() { xt_capi_set_log(nullptr, nullptr); }
    InflightLog(const InflightLog&) = delete;
    InflightLog& operator=(const InflightLog&) = delete;
    static void onLog(int level, const char* message, void* user) {
        auto* self = static_cast<InflightLog*>(user);
        if (level == 1 && std::strstr(message, "xt_gpu_target_begin_frame: ") == message) ++self->warnings;
        if (level != 0 || std::strstr(message, "xt_gpu_target: destroyed; ") != message) return;
        const char* p = std::strstr(message, "; frames in flight: ");
        if (p == nullptr) return;
        self->destroyed = true;
        self->waits = SharedLog::after(p, "frames in flight: ");
        const char* ms = std::strstr(p, " wait(s), ");
        self->waitMs = ms != nullptr ? std::strtod(ms + std::strlen(" wait(s), "), nullptr) : -1;
        self->peak = SharedLog::after(p, ", peak ");
        const char* totals = std::strchr(p + 1, ';');
        if (totals != nullptr) {
            self->submits = std::strtoul(totals + 2, nullptr, 10);
            self->passes = SharedLog::after(totals, " submits, ");
        }
    }
};

}  // namespace

// T-674 (50 §50.ac item 1): 300 frames of an animated multi-pass UI (npdo main-menu) on an offscreen target,
// no readback: begin_frame keeps at most two frames in flight (the most frames found unfinished at a
// begin_frame, read from the destroy line, is at most 2), and the frames go on without a warning.
TEST_CASE("capi gpu 16 (T-674): 300 frames keep at most two frames in flight") {
    constexpr int kSize = 256;
    InflightLog log;
    {
        Fixture f;
        xt_gpu_target* t = makeTarget(kSize, kSize);
        if (t == nullptr) return;
        xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_NPDO_BUNDLE);
        REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
        f.bundles.push_back(b);
        xt_surface_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.width = kSize;
        desc.height = kSize;
        desc.device_pixel_ratio = 1.0f;
        desc.premultiplied = 1;
        desc.run_flow = 1;  // lifecycle.shown plays the intro, rain and fog: every frame changes
        xt_surface* s = xt_surface_create(b, "main-menu", &desc);
        REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
        f.surfaces.push_back(s);
        for (int i = 0; i < 300; ++i) {
            REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(s, 1.0f / 60.0f) == XT_STATUS_OK);
            REQUIRE_MESSAGE(xt_surface_render_gpu(s, t, nullptr) == XT_STATUS_OK, xt_capi_last_error());
            REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
        }
        REQUIRE(xt_surface_detach_gpu(s) == XT_STATUS_OK);
        REQUIRE(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
    REQUIRE(log.destroyed);
    std::printf("frames in flight over 300 frames: %lu wait(s), %.3f ms, peak %lu; %lu submits, %lu passes\n",
                log.waits, log.waitMs, log.peak, log.submits, log.passes);
    CHECK(log.peak <= 2u);
    CHECK(log.warnings == 0);
    CHECK(log.waitMs >= 0.0);
    CHECK(log.submits >= 300u);
    CHECK(log.submits >= 550u);       // the surface is redrawn on (nearly) every frame, then composited
    CHECK(log.passes > log.submits);  // the fixture has frames of more than one pass
}

// T-674: a GPU that keeps up is never waited for -- every frame is read back (a blocking wait for all of
// it), so each begin_frame finds the frame two back done; xt_gpu_target_trim waits for the rest first.
TEST_CASE("capi gpu 17 (T-674): begin_frame does not wait while the GPU keeps up") {
    constexpr int kSize = 64;
    InflightLog log;
    {
        Fixture f;
        xt_gpu_target* t = makeTarget(kSize, kSize);
        if (t == nullptr) return;
        xt_surface* s = flipSurface(f, kSize);
        const xt_rect all{0, 0, kSize, kSize};
        Pixels first;
        for (int i = 0; i < 20; ++i) {
            frame(t, {{s, all}});
            const Pixels px = readback(t, kSize, kSize, kSize * 4);
            if (i == 0) first = px;
            CHECK(px == first);
        }
        REQUIRE(xt_gpu_target_trim(t) == XT_STATUS_OK);
        frame(t, {{s, all}});
        CHECK(readback(t, kSize, kSize, kSize * 4) == first);
        REQUIRE(xt_surface_detach_gpu(s) == XT_STATUS_OK);
        REQUIRE(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
    REQUIRE(log.destroyed);
    CHECK(log.waits == 0u);
    CHECK(log.peak <= 1u);
    CHECK(log.warnings == 0);
}

namespace {

/// T-675: `ui` of the npdo bundle `b` as a kShareSize surface, hidden before its first update when
/// `hidden` (destroyed by the caller).
xt_surface* npdoSurface(xt_bundle* b, const char* ui, bool hidden) {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = kShareSize;
    desc.height = kShareSize;
    desc.device_pixel_ratio = 1.0f;
    desc.premultiplied = 1;
    xt_surface* s = xt_surface_create(b, ui, &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    if (hidden) REQUIRE(xt_surface_set_visible(s, 0) == XT_STATUS_OK);
    return s;
}

}  // namespace

// T-675 (50 §50.ac item 2): two surfaces of one size on one target (npdo main-menu and settings, one
// bundle), each shown for a few frames while the other is hidden -- as a host changes scene -- and
// back, ten times, every surface drawn every frame. From the third switch on a switch creates no
// texture at all (the surface texture comes from the target's pool, the hidden surface kept its renderer,
// tables and atlases, the transients and attachments are the target's), and every show draws the bytes
// its first show drew. At 1x and 4x. T-683 (50 §50.ad item 3): at 1x the hidden surface releases its
// coverage atlas, so a show makes that surface's coverage pages again -- from the third switch on each
// switch creates exactly what the switch two before (the same surface shown) created, and at 4x (no
// coverage atlas) still nothing.
TEST_CASE("capi gpu 18 (T-675): a scene switch on one target creates no texture from the third on") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        xt_gpu_target* t = shareTarget(samples);
        if (t == nullptr) return;
        xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_NPDO_BUNDLE);
        REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
        xt_surface* s[2] = {npdoSurface(b, "main-menu", false), npdoSurface(b, "settings", true)};
        const xt_rect all{0, 0, kShareSize, kShareSize};
        auto both = [&] {
            frame(t, {{s[0], all}, {s[1], all}});
            return readback(t, kShareSize, kShareSize, kShareSize * 4);
        };
        Pixels first[2];
        first[0] = both();  // main-menu's first show
        (void)both();
        std::vector<unsigned long> created;
        int shown = 0;
        for (int sw = 1; sw <= 20; ++sw) {
            CAPTURE(sw);
            const unsigned long before = log.created;
            REQUIRE(xt_surface_set_visible(s[shown], 0) == XT_STATUS_OK);
            shown = 1 - shown;
            REQUIRE(xt_surface_set_visible(s[shown], 1) == XT_STATUS_OK);
            const Pixels px = both();
            (void)both();
            CHECK(both() == px);  // still the same a few frames later
            if (first[shown].empty()) {
                first[shown] = px;
            } else {
                CHECK(px == first[shown]);
            }
            created.push_back(log.created - before);
            if (samples == 4 && sw >= 3) CHECK(log.created == before);
            if (samples == 1 && sw >= 5) CHECK(created.back() == created[created.size() - 3]);  // T-683
        }
        std::printf("samples %d: textures created per switch:", samples);
        for (const unsigned long c : created) std::printf(" %lu", c);
        std::printf("\n");
        CHECK(log.released == 0);  // trim-on-hide never released a renderer ...
        CHECK(log.rebuilt == 0);   // ... nor was one built again
        CHECK(log.returned == 20);
        CHECK(!transparent(first[1], kShareSize, kShareSize / 2, kShareSize / 2));  // settings drew
        for (xt_surface* surface : s) xt_surface_destroy(surface);
        (void)xt_bundle_close(b);
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
}

namespace {

constexpr int kPairSize = 200;

/// T-675: a kPairSize surface of `ui` in the bundle at `dir` (the fixture closes the bundle).
xt_surface* pairSurface(Fixture& f, const char* dir, const char* ui) { return f.surface(dir, ui, kPairSize); }

/// The `side` half (0 left, 1 right) of a (2 kPairSize) x kPairSize readback.
Pixels half(const Pixels& px, int side) {
    Pixels out(static_cast<size_t>(kPairSize) * kPairSize * 4u);
    for (int y = 0; y < kPairSize; ++y) {
        std::memcpy(
            out.data() + static_cast<size_t>(y) * kPairSize * 4u,
            px.data() +
                (static_cast<size_t>(y) * 2u * kPairSize + static_cast<size_t>(side * kPairSize)) * 4u,
            static_cast<size_t>(kPairSize) * 4u);
    }
    return out;
}

}  // namespace

// T-675 (50 §50.ac item 2): two surfaces of one size visible on one target share one attachment set
// (the set count does not grow when the second is drawn; transient_bytes does not either -- the second
// surface reuses the first one's transients). The text fixture draws non-convex shapes and glyph runs
// with stencil-then-cover at 4x; flip draws a plain card with no stencil. Drawn together, in either
// order, each is byte for byte what it is drawn alone (a fresh surface each time, so every frame is
// built and drawn through the shared set): no stencil or colour of one leaks into the other.
TEST_CASE("capi gpu 19 (T-675): surfaces of one size share one attachment set without leaking") {
    for (const int samples : {4, 1}) {
        CAPTURE(samples);
        SharedLog log;
        Fixture f;
        xt_gpu_target_desc desc = offscreenDesc(2 * kPairSize, kPairSize);
        desc.sample_count = samples;
        xt_gpu_target* t = xt_gpu_target_create(&desc);
        if (t == nullptr) {
            const std::string error = xt_capi_last_error();
            REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
            std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
            return;
        }
        f.targets.push_back(t);
        const xt_rect left{0, 0, kPairSize, kPairSize}, right{kPairSize, 0, kPairSize, kPairSize};
        auto shot = [&] { return readback(t, 2 * kPairSize, kPairSize, 2 * kPairSize * 4); };
        // Each alone (the other side stays clear).
        frame(t, {{pairSurface(f, XT_CAPI_TEST_TEXT_BUNDLE, "text"), left}});
        const Pixels text = half(shot(), 0);
        const unsigned long setsOne = log.attachmentSets;
        const uint64_t transientOne = memoryStats(t).transient_bytes;
        frame(t, {{pairSurface(f, XT_CAPI_TEST_FLIP_BUNDLE, "flip"), right}});
        const Pixels card = half(shot(), 1);
        CHECK(!transparent(card, kPairSize, kPairSize / 2, kPairSize / 2));
        // Together, the stencil user first, then the other way round.
        frame(t, {{pairSurface(f, XT_CAPI_TEST_TEXT_BUNDLE, "text"), left},
                  {pairSurface(f, XT_CAPI_TEST_FLIP_BUNDLE, "flip"), right}});
        Pixels px = shot();
        CHECK(half(px, 0) == text);
        CHECK(half(px, 1) == card);
        CHECK(log.attachmentSets == setsOne);  // the composer's and the surfaces' one, as before
        CHECK(memoryStats(t).transient_bytes == transientOne);
        frame(t, {{pairSurface(f, XT_CAPI_TEST_FLIP_BUNDLE, "flip"), right},
                  {pairSurface(f, XT_CAPI_TEST_TEXT_BUNDLE, "text"), left}});
        px = shot();
        CHECK(half(px, 0) == text);
        CHECK(half(px, 1) == card);
        CHECK(log.attachmentSets == setsOne);
        std::printf("samples %d: %lu attachment set(s), transient %.2f MiB\n", samples, log.attachmentSets,
                    static_cast<double>(memoryStats(t).transient_bytes) / static_cast<double>(kMiB));
    }
}

// T-675 (50 §50.ac item 2): a hidden surface's texture waits in the target's pool for 300 frames, then
// goes; xt_gpu_target_trim empties every pool at once; xt_surface_trim also releases what trim-on-hide
// keeps, and the surface then draws the same bytes again.
TEST_CASE("capi gpu 20 (T-675): pool ageing, xt_gpu_target_trim and xt_surface_trim") {
    SharedLog log;
    Fixture f;
    xt_gpu_target* t = f.target(kShareSize, kShareSize);
    if (t == nullptr) return;
    xt_surface* s = f.surface(XT_CAPI_TEST_FLIP_BUNDLE, "flip", kShareSize);
    const xt_rect all{0, 0, kShareSize, kShareSize};
    frame(t, {{s, all}});
    const Pixels first = readback(t, kShareSize, kShareSize, kShareSize * 4);
    const uint64_t texture = uint64_t(kShareSize) * kShareSize * 4u;
    CHECK(memoryStats(t).surface_bytes == texture);
    // Hidden: the texture is idle in the pool (transient memory now), for 300 frames.
    REQUIRE(xt_surface_set_visible(s, 0) == XT_STATUS_OK);
    frame(t, {{s, all}});
    CHECK(log.returned == 1);
    xt_gpu_memory_stats m = memoryStats(t);
    CHECK(m.surface_bytes == 0);
    CHECK(m.transient_bytes >= texture);
    CHECK(log.idleSurfaceTextures == 1);
    const unsigned long frees = log.poolFrees;
    for (int i = 0; i < 290; ++i) frame(t, {{s, all}});
    CHECK(log.idleSurfaceTextures == 1);
    CHECK(log.poolFrees == frees);
    for (int i = 0; i < 20; ++i) frame(t, {{s, all}});
    CHECK(log.idleSurfaceTextures == 0);  // gone after 300 frames without use
    CHECK(log.poolFrees > frees);
    CHECK(memoryStats(t).transient_bytes < m.transient_bytes);
    // Shown again: a texture is made again, the same bytes drawn.
    unsigned long created = log.created;
    REQUIRE(xt_surface_set_visible(s, 1) == XT_STATUS_OK);
    frame(t, {{s, all}});
    CHECK(readback(t, kShareSize, kShareSize, kShareSize * 4) == first);
    CHECK(log.created > created);
    // xt_gpu_target_trim: every pooled texture goes at once (the idle surface texture, the attachment
    // sets, the transients).
    REQUIRE(xt_surface_set_visible(s, 0) == XT_STATUS_OK);
    frame(t, {{s, all}});
    CHECK(log.idleSurfaceTextures == 1);
    REQUIRE(xt_gpu_target_trim(t) == XT_STATUS_OK);
    m = memoryStats(t);
    CHECK(m.transient_bytes == 0);
    CHECK(log.idleSurfaceTextures == 0);
    CHECK(log.attachmentSets == 0);
    // xt_surface_trim on the hidden surface releases the renderer too; shown again it is built anew
    // and draws the same bytes.
    REQUIRE(xt_surface_trim(s) == XT_STATUS_OK);
    CHECK(log.released == 1);
    REQUIRE(xt_surface_set_visible(s, 1) == XT_STATUS_OK);
    frame(t, {{s, all}});
    CHECK(readback(t, kShareSize, kShareSize, kShareSize * 4) == first);
    CHECK(log.rebuilt == 1);
    // Ten switches later nothing more was made than the first show after the trim (the texture back
    // from the pool, the renderer kept).
    frame(t, {{s, all}});
    created = log.created;
    for (int i = 0; i < 10; ++i) {
        REQUIRE(xt_surface_set_visible(s, 0) == XT_STATUS_OK);
        frame(t, {{s, all}});
        REQUIRE(xt_surface_set_visible(s, 1) == XT_STATUS_OK);
        frame(t, {{s, all}});
        CHECK(readback(t, kShareSize, kShareSize, kShareSize * 4) == first);
    }
    CHECK(log.created == created);
}

namespace {

constexpr int kBudgetSize = 1024;

/// T-678: an offscreen kBudgetSize target with `samples`, kept by `f`; null without an adapter.
xt_gpu_target* budgetTarget(Fixture& f, int samples) {
    xt_gpu_target_desc desc = offscreenDesc(kBudgetSize, kBudgetSize);
    desc.sample_count = samples;
    xt_gpu_target* t = xt_gpu_target_create(&desc);
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
        std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
        return nullptr;
    }
    f.targets.push_back(t);
    return t;
}

}  // namespace

// T-678 (50 §50.ac addendum item 1): the target's transient pool keeps what the last target frame
// borrowed plus 16 MiB. Scene A is fixtures/effects (nested translucent layers, blend modes, shadows,
// glows, shine), whose first frame -- every effect cache being filled -- borrows far more than 16 MiB of
// transients; scene B is the flip card (no effect). Every surface is drawn every frame, the hidden one
// too, as a host does. A few frames after the switch to B the pool -- transient_bytes without the idle 1x
// surface textures, which age by their own rule -- holds no more than B's own use (measured before A was
// ever shown) plus 16 MiB, where T-675's budget (two full-surface entries, at least 64 MiB) kept A's.
// Back to A, it draws the bytes it drew before. At 1x and 4x.
TEST_CASE("capi gpu 21 (T-678): after a switch the transient pool shrinks to the new scene's use + 16 MiB") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        Fixture f;
        xt_gpu_target* t = budgetTarget(f, samples);
        if (t == nullptr) return;
        xt_surface* a = f.surface(XT_CAPI_TEST_EFFECTS_BUNDLE, "effects", kBudgetSize);
        xt_surface* b = f.surface(XT_CAPI_TEST_FLIP_BUNDLE, "flip", kBudgetSize);
        const xt_rect all{0, 0, kBudgetSize, kBudgetSize};
        const uint64_t texture = uint64_t(kBudgetSize) * kBudgetSize * 4u;
        auto draw = [&] { frame(t, {{a, all}, {b, all}}); };
        auto shot = [&] { return readback(t, kBudgetSize, kBudgetSize, kBudgetSize * 4); };
        auto pool = [&] { return memoryStats(t).transient_bytes - log.idleSurfaceTextures * texture; };
        auto show = [&](bool sceneA) {
            REQUIRE(xt_surface_set_visible(a, sceneA ? 1 : 0) == XT_STATUS_OK);
            REQUIRE(xt_surface_set_visible(b, sceneA ? 0 : 1) == XT_STATUS_OK);
        };
        show(false);
        for (int i = 0; i < 3; ++i) draw();
        const uint64_t useB = pool();
        show(true);
        draw();
        const uint64_t useA = pool();
        draw();
        draw();
        const Pixels pxA = shot();
        std::printf("samples %d: pool on B %.2f MiB, after A's first frame %.2f MiB", samples,
                    static_cast<double>(useB) / static_cast<double>(kMiB),
                    static_cast<double>(useA) / static_cast<double>(kMiB));
        REQUIRE(useA > useB + 16 * kMiB);  // otherwise the check below would hold without the budget
        show(false);
        for (int i = 0; i < 3; ++i) draw();
        std::printf(", back on B %.2f MiB\n", static_cast<double>(pool()) / static_cast<double>(kMiB));
        CHECK(pool() <= useB + 16 * kMiB);
        show(true);
        draw();
        CHECK(shot() == pxA);
        draw();
        CHECK(shot() == pxA);
    }
}

// T-678 (50 §50.ac addendum item 1): the budget never makes a steady scene rebuild its transients. The
// three effect-heaviest fixtures of the existing bundles (game-ui hud and pause, hud-overlay hud:
// shadows, glows, shine, backdrop blurs, blend modes), each a surface of its own on one target, drawn
// together frame after frame: from the third frame on no texture at all is created (the transients
// included). At 1x and 4x.
TEST_CASE("capi gpu 22 (T-678): a steady multi-effect scene creates no transient from the third frame on") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        Fixture f;
        xt_gpu_target* t = budgetTarget(f, samples);
        if (t == nullptr) return;
        xt_surface* s[3] = {f.surface(XT_CAPI_TEST_BUNDLE, "hud", kBudgetSize),
                            f.surface(XT_CAPI_TEST_BUNDLE, "pause", kBudgetSize),
                            f.surface(XT_CAPI_TEST_PORTS_BUNDLE, "hud", kBudgetSize)};
        const xt_rect all{0, 0, kBudgetSize, kBudgetSize};
        std::printf("samples %d: textures created per frame:", samples);
        for (int i = 1; i <= 10; ++i) {
            CAPTURE(i);
            const unsigned long before = log.created;
            frame(t, {{s[0], all}, {s[1], all}, {s[2], all}});
            std::printf(" %lu", log.created - before);
            if (i >= 3) CHECK(log.created == before);
        }
        std::printf("\n");
    }
}

namespace {

constexpr int kCardsSize = 128;

/// T-683: an offscreen kCardsSize target with `samples` and the fixtures/boxshadow `cards` surface, both
/// kept by `f`; the target is null without an adapter.
xt_gpu_target* cardsTarget(Fixture& f, int samples, xt_surface*& cards) {
    xt_gpu_target_desc desc = offscreenDesc(kCardsSize, kCardsSize);
    desc.sample_count = samples;
    xt_gpu_target* t = xt_gpu_target_create(&desc);
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
        std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
        return nullptr;
    }
    f.targets.push_back(t);
    cards = f.surface(XT_CAPI_TEST_BOXSHADOW_BUNDLE, "cards", kCardsSize);
    return t;
}

}  // namespace

// T-683 (50 §50.ad item 2): xt_gpu_target_create prewarms every built-in program (one info line with the
// shader and pipeline counts), so the first frames of the first surface that draws box shadows -- one
// term and three, the two programs -- compile no shader. At 1x and 4x.
TEST_CASE("capi gpu 23 (T-683): after the prewarm the first box-shadow frame compiles no shader") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        Fixture f;
        xt_surface* cards = nullptr;
        xt_gpu_target* t = cardsTarget(f, samples, cards);
        if (t == nullptr) return;
        REQUIRE(log.prewarms == 1);
        std::printf("samples %d: prewarmed %lu shader(s), %lu pipeline(s)\n", samples, log.prewarmedShaders,
                    log.prewarmedPipelines);
        CHECK(log.prewarmedShaders >= 30);  // the 28 programs, the blit and the mip box step
        CHECK(log.prewarmedPipelines >= log.prewarmedShaders);
        const xt_rect all{0, 0, kCardsSize, kCardsSize};
        for (int i = 0; i < 3; ++i) {
            frame(t, {{cards, all}});
            REQUIRE(log.lines > 0);
            CHECK(log.shaderCompiles == 0);
        }
        std::printf("samples %d: pipelines created after the prewarm: %lu\n", samples, log.pipelineCreates);
    }
}

// T-683 (50 §50.ad item 3): trim-on-hide releases the surface's coverage atlas (pages, CPU copies, keys) --
// atlas_bytes falls by at least one page -- and shown again the surface rasterizes it anew and draws the
// bytes it drew before, with atlas_bytes back where it was. At 1x (4x draws no coverage atlas).
TEST_CASE("capi gpu 24 (T-683): a hidden surface drops its coverage atlas and draws the same bytes again") {
    SharedLog log;
    Fixture f;
    xt_surface* cards = nullptr;
    xt_gpu_target* t = cardsTarget(f, 1, cards);
    if (t == nullptr) return;
    const xt_rect all{0, 0, kCardsSize, kCardsSize};
    frame(t, {{cards, all}});
    frame(t, {{cards, all}});
    const Pixels before = readback(t, kCardsSize, kCardsSize, kCardsSize * 4);
    const uint64_t shown = memoryStats(t).atlas_bytes;
    REQUIRE(xt_surface_set_visible(cards, 0) == XT_STATUS_OK);
    frame(t, {{cards, all}});
    frame(t, {{cards, all}});
    CHECK(log.returned == 1);
    const uint64_t hidden = memoryStats(t).atlas_bytes;
    std::printf("atlas_bytes shown %llu, hidden %llu\n", static_cast<unsigned long long>(shown),
                static_cast<unsigned long long>(hidden));
    CHECK(hidden + 512u * 512u <= shown);  // at least the first coverage page (512 x 512 R8)
    REQUIRE(xt_surface_set_visible(cards, 1) == XT_STATUS_OK);
    frame(t, {{cards, all}});
    CHECK(readback(t, kCardsSize, kCardsSize, kCardsSize * 4) == before);
    CHECK(memoryStats(t).atlas_bytes == shown);
}

// T-666 (10 §10.11 item 8): a desc whose sample_count is 0 makes a 1x target. The text fixture (stencil
// user) drawn on targets of sample_count 0, 1 and 4: at 0 the target's pools hold what they hold at 1
// (xt_gpu_memory_stats.transient_bytes: no 4x colour and stencil attachment set, which 4 adds) and
// the bytes drawn are those of 1.
TEST_CASE("capi gpu 25 (T-666): sample_count 0 makes a 1x target") {
    struct Result {
        uint64_t transient = 0;
        Pixels px;
    };
    Result results[3];
    const int counts[3] = {0, 1, 4};
    for (int i = 0; i < 3; ++i) {
        CAPTURE(counts[i]);
        Fixture f;
        xt_gpu_target_desc desc = offscreenDesc(kPairSize, kPairSize);
        desc.sample_count = counts[i];
        xt_gpu_target* t = xt_gpu_target_create(&desc);
        if (t == nullptr) {
            const std::string error = xt_capi_last_error();
            REQUIRE_MESSAGE(contains(error.c_str(), "no WebGPU adapter"), error);
            std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
            return;
        }
        f.targets.push_back(t);
        xt_surface* s = pairSurface(f, XT_CAPI_TEST_TEXT_BUNDLE, "text");
        const xt_rect all{0, 0, kPairSize, kPairSize};
        frame(t, {{s, all}});
        frame(t, {{s, all}});
        results[i].transient = memoryStats(t).transient_bytes;
        results[i].px = readback(t, kPairSize, kPairSize, kPairSize * 4);
        std::printf("sample_count %d: transient %llu bytes\n", counts[i],
                    static_cast<unsigned long long>(results[i].transient));
    }
    CHECK(results[0].transient == results[1].transient);
    CHECK(results[0].transient < results[2].transient);
    CHECK(results[0].px == results[1].px);
}

// T-685 (50 §50.ac addendum 2): one visible surface alternates, period after period of 300 frames, between
// fixtures/effects with its nested panels (`heavy`, 150 frames) and without them (150 frames) -- the
// heavy state borrows many more transients than the light one, and an unchanged frame borrows none (it is
// not drawn again), so T-678's budget alone evicted the heavy state's transients in every period and the
// next period made them again. Their last borrower is the visible surface: from the second period on no
// texture at all is created. Then the surface is hidden and the flip card shown instead: a few frames later
// no transient is protected any more and the pool holds no more than the card's own use plus 16 MiB (the
// transient budget, case 21) plus 16 MiB -- the released effect-cache targets of the hidden surface, kept
// under their own unchanged budget (kCachedFreeBudgetBytes), count in transient_bytes as well; T-678's
// code ends this sequence at the same bytes. At 1x and 4x.
TEST_CASE(
    "capi gpu 26 (T-685): a visible surface alternating states creates no texture from its second period") {
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        SharedLog log;
        Fixture f;
        xt_gpu_target* t = budgetTarget(f, samples);
        if (t == nullptr) return;
        xt_surface* a = f.surface(XT_CAPI_TEST_EFFECTS_BUNDLE, "effects", kBudgetSize);
        xt_surface* b = f.surface(XT_CAPI_TEST_FLIP_BUNDLE, "flip", kBudgetSize);
        const uint32_t heavy = xt_surface_port_lookup(a, "heavy");
        REQUIRE(heavy != 0);
        const xt_rect all{0, 0, kBudgetSize, kBudgetSize};
        const uint64_t texture = uint64_t(kBudgetSize) * kBudgetSize * 4u;
        auto draw = [&] { frame(t, {{a, all}, {b, all}}); };
        auto pool = [&] { return memoryStats(t).transient_bytes - log.idleSurfaceTextures * texture; };
        auto show = [&](bool sceneA) {
            REQUIRE(xt_surface_set_visible(a, sceneA ? 1 : 0) == XT_STATUS_OK);
            REQUIRE(xt_surface_set_visible(b, sceneA ? 0 : 1) == XT_STATUS_OK);
        };
        show(false);
        for (int i = 0; i < 3; ++i) draw();
        const uint64_t useB = pool();
        show(true);
        constexpr int kPeriod = 300;
        std::vector<unsigned long> created;
        for (int period = 1; period <= 4; ++period) {
            CAPTURE(period);
            const unsigned long before = log.created;
            REQUIRE(xt_surface_port_set_bool(a, heavy, 1) == XT_STATUS_OK);
            for (int i = 0; i < kPeriod / 2; ++i) draw();
            REQUIRE(xt_surface_port_set_bool(a, heavy, 0) == XT_STATUS_OK);
            for (int i = 0; i < kPeriod / 2; ++i) draw();
            created.push_back(log.created - before);
            if (period >= 2) CHECK(log.created == before);
        }
        std::printf("samples %d: textures created per period:", samples);
        for (const unsigned long c : created) std::printf(" %lu", c);
        std::printf("; protected %lu KiB, budget evictions %lu", log.protectedKiB, log.budgetEvictions);
        CHECK(log.protectedKiB > 0);
        show(false);
        for (int i = 0; i < 3; ++i) draw();
        std::printf("; pool on B %.2f MiB, after hiding A %.2f MiB\n",
                    static_cast<double>(useB) / static_cast<double>(kMiB),
                    static_cast<double>(pool()) / static_cast<double>(kMiB));
        CHECK(log.protectedKiB == 0);
        CHECK(pool() <= useB + 16 * kMiB + 16 * kMiB);
    }
}

// T-685 (50 §50.ac addendum 2): the protection is from the budget only. A visible surface leaves its heavy
// state (fixtures/effects `heavy` false) and stays shown: its heavy transients stay in the pool -- no
// budget eviction, transient_bytes unchanged -- for 1700 frames, and by 1800 + 100 frames the ageing has
// destroyed them (and the protected bytes fall).
TEST_CASE("capi gpu 27 (T-685): protected transients a visible surface no longer uses still age out") {
    SharedLog log;
    Fixture f;
    xt_gpu_target* t = budgetTarget(f, 1);
    if (t == nullptr) return;
    xt_surface* a = f.surface(XT_CAPI_TEST_EFFECTS_BUNDLE, "effects", kBudgetSize);
    const uint32_t heavy = xt_surface_port_lookup(a, "heavy");
    REQUIRE(heavy != 0);
    const xt_rect all{0, 0, kBudgetSize, kBudgetSize};
    auto draw = [&] { frame(t, {{a, all}}); };
    for (int i = 0; i < 3; ++i) draw();
    const unsigned long protectedHeavy = log.protectedKiB;
    REQUIRE(protectedHeavy > 0);
    REQUIRE(xt_surface_port_set_bool(a, heavy, 0) == XT_STATUS_OK);
    draw();
    const uint64_t poolLight = memoryStats(t).transient_bytes;
    const unsigned long budgetEvictions = log.budgetEvictions, ageEvictions = log.ageEvictions;
    for (int i = 0; i < 1700; ++i) draw();
    CHECK(memoryStats(t).transient_bytes == poolLight);
    CHECK(log.budgetEvictions == budgetEvictions);
    CHECK(log.ageEvictions == ageEvictions);
    for (int i = 0; i < 200; ++i) draw();
    std::printf("protected %lu KiB -> %lu KiB, transient %.2f -> %.2f MiB, age evictions %lu\n",
                protectedHeavy, log.protectedKiB, static_cast<double>(poolLight) / static_cast<double>(kMiB),
                static_cast<double>(memoryStats(t).transient_bytes) / static_cast<double>(kMiB),
                log.ageEvictions - ageEvictions);
    CHECK(log.ageEvictions > ageEvictions);
    CHECK(log.protectedKiB < protectedHeavy);
    CHECK(memoryStats(t).transient_bytes < poolLight);
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
