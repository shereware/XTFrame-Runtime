// T-642 (Docs/design/23-host-integration.md §23.3, §23.2 revision "SW 局部重繪補強"): a partial redraw
// is byte-identical to a full one on real demo scenes, and costs no more. Each case drives two
// surfaces of the same packed bundle UI through the same frames (same dt, same port writes):
//   P renders into one buffer every frame, so after frame 0 xt_surface_render redraws only the damage;
//   F renders into two buffers in turn, so every frame is a new target and a full redraw.
// After every frame the two outputs must be equal byte for byte. The scenes are the ones T-639
// measured (Docs/reviews/2026-11-07-sw-render-perf.md): npdo `hud` and `settings` (`active` = 1 plays
// their intro: full-screen background image, vignette gradient, small animated text and icons) and
// game-ui `hud` (radar sweep, cooldowns, drop-shadowed cards). The summed renderer time of P (frames
// after the first) must not exceed F's.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

/// A Debug SW frame is tens of times slower than a Release one: a Debug build renders the scenes at
/// half size (device pixel ratio 0.5, the same layout), a Release one at the design size.
#ifdef NDEBUG
constexpr int kW = 1280;
constexpr int kH = 720;
constexpr float kDpr = 1.0f;
constexpr int kFrames = 120;
#else
constexpr int kW = 640;
constexpr int kH = 360;
constexpr float kDpr = 0.5f;
constexpr int kFrames = 60;
#endif
constexpr float kDt = 1.0f / 60.0f;

using Pixels = std::vector<uint8_t>;

xt_surface* makeSurface(xt_bundle* b, const char* ui, const char* port) {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = kW;
    desc.height = kH;
    desc.device_pixel_ratio = kDpr;
    desc.premultiplied = 1;
    desc.run_flow = 1;
    desc.system_fonts = 1;  // The scenes' text is drawn too (the bitmap glyph path).
    xt_surface* s = xt_surface_create(b, ui, &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    xt_surface_set_perf_enabled(s, 1);
    if (port != nullptr) {
        const uint32_t id = xt_surface_port_lookup(s, port);
        REQUIRE(id != XT_INVALID_PORT);
        REQUIRE(xt_surface_port_set_text(s, id, "1") == XT_STATUS_OK);
    }
    return s;
}

float renderMs(const xt_surface* s) {
    xt_frame_stats fs{};
    REQUIRE(xt_surface_frame_stats(s, &fs) == XT_STATUS_OK);
    return fs.render_ms;
}

/// Runs one scene; `port` (or null) is set to "1" on both surfaces before the first frame.
void checkScene(const char* bundleDir, const char* ui, const char* port) {
    xt_bundle* b = xt_bundle_open(bundleDir);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    xt_surface* partial = makeSurface(b, ui, port);
    xt_surface* full = makeSurface(b, ui, port);
    const size_t bytes = static_cast<size_t>(kW) * kH * 4u;
    Pixels partialBuf(bytes, 0);
    Pixels fullBuf[2] = {Pixels(bytes, 0), Pixels(bytes, 0)};
    double partialMs = 0.0;
    double fullMs = 0.0;
    int partialFrames = 0;
    for (int f = 0; f < kFrames; ++f) {
        const float dt = f == 0 ? 0.0f : kDt;
        REQUIRE(xt_surface_update(partial, dt) == XT_STATUS_OK);
        REQUIRE(xt_surface_update(full, dt) == XT_STATUS_OK);
        xt_damage pd{};
        REQUIRE(xt_surface_render(partial, partialBuf.data(), kW * 4, &pd) == XT_STATUS_OK);
        Pixels& fb = fullBuf[f & 1];
        xt_damage fd{};
        REQUIRE(xt_surface_render(full, fb.data(), kW * 4, &fd) == XT_STATUS_OK);
        CHECK(fd.full == 1);
        if (f > 0) {
            partialMs += renderMs(partial);
            fullMs += renderMs(full);
            if (pd.full == 0 && pd.changed != 0) {
                ++partialFrames;
            }
        }
        if (partialBuf != fb) {
            size_t i = 0;
            while (i < bytes && partialBuf[i] == fb[i]) {
                ++i;
            }
            const size_t px = i / 4;
            FAIL_CHECK("frame " << f << ": partial differs from full at pixel (" << px % kW << ", " << px / kW
                                << ") channel " << i % 4 << ": partial " << int(partialBuf[i]) << ", full "
                                << int(fb[i]));
            break;
        }
    }
    std::printf(
        "T-642 %s/%s: %d partial frames of %d; renderer partial %.3f ms, full %.3f ms (frames 1..%d)\n",
        bundleDir, ui, partialFrames, kFrames - 1, partialMs, fullMs, kFrames - 1);
    CHECK(partialFrames > 0);  // The scene must actually exercise the partial path.
    CHECK(partialMs <= fullMs);
    xt_surface_destroy(full);
    xt_surface_destroy(partial);
    (void)xt_bundle_close(b);
}

/// T-644 (§23.2 revision "步驟 2 改為多區間"): the mean dirty-area fraction of the viewport over the
/// frames that changed (frame 0 excluded; a `full` frame counts as 1), and the same without the
/// rectangles lying inside `excluded` (device pixels).
struct DamageStats {
    double fraction = 0.0;
    double fractionOutside = 0.0;
};

DamageStats damageStats(const char* bundleDir, const char* ui, const char* port, xt_rect excluded) {
    xt_bundle* b = xt_bundle_open(bundleDir);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    xt_surface* s = makeSurface(b, ui, port);
    Pixels buf(static_cast<size_t>(kW) * kH * 4u, 0);
    const double viewport = static_cast<double>(kW) * kH;
    DamageStats sum;
    double msSum = 0.0;
    int changedFrames = 0;
    int fullFrames = 0;
    for (int f = 0; f < kFrames; ++f) {
        REQUIRE(xt_surface_update(s, f == 0 ? 0.0f : kDt) == XT_STATUS_OK);
        xt_damage d{};
        REQUIRE(xt_surface_render(s, buf.data(), kW * 4, &d) == XT_STATUS_OK);
        if (f == 0 || d.changed == 0) {
            continue;
        }
        ++changedFrames;
        msSum += renderMs(s);
        if (d.full != 0) {
            ++fullFrames;
            sum.fraction += 1.0;
            sum.fractionOutside += 1.0;
            continue;
        }
        for (uint32_t i = 0; i < d.rect_count; ++i) {
            const xt_rect& r = d.rects[i];
            const double area = static_cast<double>(r.w) * static_cast<double>(r.h) / viewport;
            sum.fraction += area;
            const bool inside = r.x >= excluded.x && r.y >= excluded.y &&
                                r.x + r.w <= excluded.x + excluded.w && r.y + r.h <= excluded.y + excluded.h;
            sum.fractionOutside += inside ? 0.0 : area;
        }
    }
    DamageStats mean;
    if (changedFrames > 0) {
        mean.fraction = sum.fraction / changedFrames;
        mean.fractionOutside = sum.fractionOutside / changedFrames;
    }
    std::printf(
        "T-644 %s/%s: %d changed frames (%d full), mean dirty area %.2f %% (%.2f %% outside the excluded "
        "rect), mean renderer %.3f ms\n",
        bundleDir, ui, changedFrames, fullFrames, mean.fraction * 100.0, mean.fractionOutside * 100.0,
        changedFrames > 0 ? msSum / changedFrames : 0.0);
    xt_surface_destroy(s);
    (void)xt_bundle_close(b);
    return mean;
}

}  // namespace

TEST_CASE("capi partial 1 (T-642): npdo hud -- partial redraw equals a full redraw, byte for byte") {
    checkScene(XT_CAPI_TEST_NPDO_BUNDLE, "hud", "active");
}

TEST_CASE("capi partial 2 (T-642): npdo settings -- partial redraw equals a full redraw, byte for byte") {
    checkScene(XT_CAPI_TEST_NPDO_BUNDLE, "settings", "active");
}

TEST_CASE("capi partial 3 (T-642): game-ui hud -- partial redraw equals a full redraw, byte for byte") {
    checkScene(XT_CAPI_TEST_BUNDLE, "hud", nullptr);
}

TEST_CASE(
    "capi partial 4 (T-644): game-ui hud -- only the animated parts are dirty, not the cards between them") {
    // Radar sweep, Q cooldown glow and the timer text animate; the static cards drawn between them stayed
    // dirty under the single first-to-last-differing range (24.6 % of the screen on average). The HP bar
    // also shrinks every 0.4 s (hud.xtflow `bleed`) inside the player panel, whose drop shadow is an open
    // PushEffect: B6 keeps that panel's whole halo (0, 0, 360, 236 at the design size, 9.2 % of the
    // screen) dirty in those frames, ~5.9 % of the average alone.
    const int s = static_cast<int>(kDpr * 2.0f);  // 2 at the design size, 1 at half size
    const xt_rect playerHalo{0, 0, 180 * s, 118 * s};
    const DamageStats hud = damageStats(XT_CAPI_TEST_BUNDLE, "hud", nullptr, playerHalo);
    CHECK(hud.fraction > 0.0);
    CHECK(hud.fraction <= 0.08);
    CHECK(hud.fractionOutside <= 0.025);
    // npdo settings: printed for the before/after record only (its changes are whole fading panels).
    (void)damageStats(XT_CAPI_TEST_NPDO_BUNDLE, "settings", "active", xt_rect{0, 0, 0, 0});
}
