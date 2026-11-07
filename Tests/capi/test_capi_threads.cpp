// T-652 (Docs/design/50-c-abi.md §50.1 "跨 bundle 的執行緒"): one thread per bundle. Two different
// bundles, one surface each, each driven by its own std::thread through 500 frames of input + update +
// render, at the same time. Every frame's pixels are folded into a hash; the threaded run must give the
// same hashes as the same two scenes run one after the other on the calling thread (a race that does
// not crash still shows up as different pixels). XTAtrium's report: two such threads crashed in
// xt_surface_update (0xC0000005), from the function-local `static` scratch vectors of the style engine,
// the world-transform pass and the painter's child sort that every Context shared.

#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

constexpr int kFrames = 500;
constexpr float kDt = 1.0f / 60.0f;

/// One surface of one bundle, opened, driven and closed on whichever thread calls run().
struct Scene {
    const char* bundleDir;
    const char* ui;
    int width;
    int height;
    float dpr;
    int runFlow;
    int systemFonts;
    const char* activePort;  // set to "1" before the first frame, or null
    bool clicks;             // a click at the centre every 40 frames (flip toggles `flipped`)
};

/// What one run of a scene gives: the hash of every frame's pixels folded together, and the first
/// failure (empty when none). doctest's assertions are not used off the main thread.
struct Outcome {
    uint64_t hash = 1469598103934665603ull;
    std::string error;
};

void fold(uint64_t& hash, const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    }
}

xt_input_event pointerEvent(int kind, float x, float y) {
    xt_input_event e{};
    e.struct_size = sizeof(e);
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.clicks = 1;
    return e;
}

bool push(xt_surface* s, int kind, float x, float y, Outcome& out) {
    const xt_input_event e = pointerEvent(kind, x, y);
    if (xt_surface_push_input(s, &e) != XT_STATUS_OK) {
        out.error = std::string("push_input: ") + xt_capi_last_error();
        return false;
    }
    return true;
}

/// Pointer input of frame `f`: a sweep over the surface (hover), a wheel notch every 25 frames and,
/// with `clicks`, a click at the centre every 40.
bool frameInput(const Scene& scene, xt_surface* s, int f, Outcome& out) {
    const float w = static_cast<float>(scene.width) / scene.dpr;
    const float h = static_cast<float>(scene.height) / scene.dpr;
    const float x = w * static_cast<float>(f % 50) / 50.0f;
    const float y = h * static_cast<float>((f / 50) % 10) / 10.0f;
    if (!push(s, XT_INPUT_POINTER_MOVE, x, y, out)) {
        return false;
    }
    if (f % 25 == 12) {
        xt_input_event wheel = pointerEvent(XT_INPUT_WHEEL, x, y);
        wheel.wheel_y = (f / 25) % 2 == 0 ? 1.0f : -1.0f;
        if (xt_surface_push_input(s, &wheel) != XT_STATUS_OK) {
            out.error = std::string("push_input wheel: ") + xt_capi_last_error();
            return false;
        }
    }
    if (scene.clicks && f % 40 == 20) {
        return push(s, XT_INPUT_POINTER_MOVE, w * 0.5f, h * 0.5f, out) &&
               push(s, XT_INPUT_POINTER_DOWN, w * 0.5f, h * 0.5f, out) &&
               push(s, XT_INPUT_POINTER_UP, w * 0.5f, h * 0.5f, out);
    }
    return true;
}

bool drive(const Scene& scene, xt_bundle* b, Outcome& out) {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = scene.width;
    desc.height = scene.height;
    desc.device_pixel_ratio = scene.dpr;
    desc.premultiplied = 1;
    desc.run_flow = scene.runFlow;
    desc.system_fonts = scene.systemFonts;
    xt_surface* s = xt_surface_create(b, scene.ui, &desc);
    if (s == nullptr) {
        out.error = std::string("surface_create: ") + xt_capi_last_error();
        return false;
    }
    bool ok = true;
    if (scene.activePort != nullptr) {
        const uint32_t id = xt_surface_port_lookup(s, scene.activePort);
        ok = id != XT_INVALID_PORT && xt_surface_port_set_text(s, id, "1") == XT_STATUS_OK;
        if (!ok) {
            out.error = std::string("port ") + scene.activePort;
        }
    }
    const int stride = scene.width * 4;
    std::vector<uint8_t> rgba(static_cast<size_t>(stride) * static_cast<size_t>(scene.height), 0);
    for (int f = 0; ok && f < kFrames; ++f) {
        xt_damage damage{};
        ok = frameInput(scene, s, f, out);
        if (ok && xt_surface_update(s, f == 0 ? 0.0f : kDt) != XT_STATUS_OK) {
            out.error = std::string("update: ") + xt_capi_last_error();
            ok = false;
        }
        if (ok && xt_surface_render(s, rgba.data(), stride, &damage) != XT_STATUS_OK) {
            out.error = std::string("render: ") + xt_capi_last_error();
            ok = false;
        }
        if (ok) {
            fold(out.hash, rgba.data(), rgba.size());
        }
    }
    xt_surface_destroy(s);
    return ok;
}

Outcome run(const Scene& scene) {
    Outcome out;
    xt_bundle* b = xt_bundle_open(scene.bundleDir);
    if (b == nullptr) {
        out.error = std::string("bundle_open: ") + xt_capi_last_error();
        return out;
    }
    (void)drive(scene, b, out);
    if (xt_bundle_close(b) != XT_STATUS_OK && out.error.empty()) {
        out.error = std::string("bundle_close: ") + xt_capi_last_error();
    }
    return out;
}

/// Runs `a` and `b` one after the other on this thread, then each on its own thread at the same time,
/// and checks both threaded runs against the serial ones.
void checkPair(const Scene& a, const Scene& b) {
    const Outcome serialA = run(a);
    const Outcome serialB = run(b);
    REQUIRE_MESSAGE(serialA.error.empty(), serialA.error);
    REQUIRE_MESSAGE(serialB.error.empty(), serialB.error);
    Outcome threadedA;
    Outcome threadedB;
    std::thread ta([&] { threadedA = run(a); });
    std::thread tb([&] { threadedB = run(b); });
    ta.join();
    tb.join();
    CHECK_MESSAGE(threadedA.error.empty(), threadedA.error);
    CHECK_MESSAGE(threadedB.error.empty(), threadedB.error);
    CHECK(threadedA.hash == serialA.hash);
    CHECK(threadedB.hash == serialB.hash);
}

/// A Debug (and ASan) SW frame is tens of times slower than a Release one: npdo renders at a fifth of
/// its design size there (the same layout), at a quarter in Release.
#ifdef NDEBUG
constexpr int kNpdoW = 480;
constexpr int kNpdoH = 270;
constexpr float kNpdoDpr = 0.25f;
#else
constexpr int kNpdoW = 384;
constexpr int kNpdoH = 216;
constexpr float kNpdoDpr = 0.2f;
#endif

const Scene kFlip{XT_CAPI_TEST_FLIP_BUNDLE, "flip", 64, 64, 1.0f, 1, 0, nullptr, true};
const Scene kState{XT_CAPI_TEST_STATE_BUNDLE, "state", 64, 64, 1.0f, 0, 0, nullptr, false};
const Scene kNpdoSettings{
    XT_CAPI_TEST_NPDO_BUNDLE, "settings", kNpdoW, kNpdoH, kNpdoDpr, 1, 1, "active", true};
const Scene kNpdoHud{XT_CAPI_TEST_NPDO_BUNDLE, "hud", kNpdoW, kNpdoH, kNpdoDpr, 1, 1, "active", false};

}  // namespace

TEST_CASE(
    "capi threads 1 (T-652): flip and state bundles on two threads, 500 frames each, equal the serial run") {
    checkPair(kFlip, kState);
}

TEST_CASE(
    "capi threads 2 (T-652): two npdo bundles (settings / hud, text and images) on two threads equal the "
    "serial run") {
    checkPair(kNpdoSettings, kNpdoHud);
}

TEST_CASE("capi threads 3 (T-652): npdo settings and flip on two threads equal the serial run") {
    checkPair(kNpdoSettings, kFlip);
}
