// T-635 (Docs/design/50-c-abi.md §50.y): xt_surface_save_state / xt_surface_load_state, the C ABI
// face of Document::snapshotState / restoreState. XT_CAPI_TEST_STATE_BUNDLE is
// Tests/capi/fixtures/state, packed at build time: a 64x64 UI of four 16px rows -- #Bar (width bound
// to the `hp` port), #Level (custom states Warn yellow / Ok cyan), #Pulse (the 2s `pulse` timeline
// fades it in) and #Box (a scrolled box of four colored stripes).

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

constexpr int kSize = 64;

xt_surface_desc stateDesc() {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = kSize;
    desc.height = kSize;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 0;
    return desc;
}

/// The state bundle and one surface of its `state` UI; destroyed / closed at scope end.
struct StateSurface {
    xt_bundle* b = nullptr;
    xt_surface* s = nullptr;
    StateSurface() {
        b = xt_bundle_open(XT_CAPI_TEST_STATE_BUNDLE);
        REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
        const xt_surface_desc desc = stateDesc();
        s = xt_surface_create(b, "state", &desc);
        REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    }
    ~StateSurface() {
        if (s != nullptr) {
            xt_surface_destroy(s);
        }
        if (b != nullptr) {
            (void)xt_bundle_close(b);
        }
    }
    StateSurface(const StateSurface&) = delete;
    StateSurface& operator=(const StateSurface&) = delete;
};

/// One RGBA8 buffer per surface, kept across frames (a frame redraws only its damage).
using Frame = std::vector<uint8_t>;
Frame newFrame() { return Frame(static_cast<size_t>(kSize) * kSize * 4u, 0); }

void updateAndRender(xt_surface* s, Frame& rgba, float dt = 0.0f) {
    xt_damage damage{};
    REQUIRE(xt_surface_update(s, dt) == XT_STATUS_OK);
    REQUIRE(xt_surface_render(s, rgba.data(), kSize * 4, &damage) == XT_STATUS_OK);
}

/// A wheel over #Box (the bottom row).
void wheel(xt_surface* s, float notchesY) {
    xt_input_event move{};
    move.struct_size = sizeof(move);
    move.kind = XT_INPUT_POINTER_MOVE;
    move.x = 32.0f;
    move.y = 56.0f;
    move.gesture_scale = 1.0f;
    REQUIRE(xt_surface_push_input(s, &move) == XT_STATUS_OK);
    xt_input_event e = move;
    e.kind = XT_INPUT_WHEEL;
    e.wheel_y = notchesY;
    REQUIRE(xt_surface_push_input(s, &e) == XT_STATUS_OK);
}

/// The state every case saves: hp 25, Level Warn, `pulse` at 1s (opacity 0.5), #Box scrolled.
std::string setUpAndSave(xt_surface* s, Frame& rgba) {
    updateAndRender(s, rgba);
    const uint32_t hp = xt_surface_port_lookup(s, "hp");
    REQUIRE(hp != XT_INVALID_PORT);
    REQUIRE(xt_surface_port_set_number(s, hp, 25.0) == XT_STATUS_OK);
    REQUIRE(xt_surface_connection_pulse(s, "#Level.setState", "Warn") == XT_STATUS_OK);
    REQUIRE(xt_surface_play_timeline(s, "", "pulse", nullptr) == XT_STATUS_OK);
    REQUIRE(xt_surface_seek_timeline(s, "", "pulse", 1.0f) == XT_STATUS_OK);
    wheel(s, 1.0f);  // 40px down (08: one notch = 40px).
    updateAndRender(s, rgba);
    const char* saved = xt_surface_save_state(s);
    REQUIRE_MESSAGE(saved != nullptr, xt_capi_last_error());
    const std::string json = saved;
    INFO(json);
    REQUIRE(json.find("\"hp\"") != std::string::npos);
    REQUIRE(json.find("Warn") != std::string::npos);
    REQUIRE(json.find("pulse") != std::string::npos);
    REQUIRE(json.find("\"Box\"") != std::string::npos);  // Scrolled: listed with its offset.
    return json;
}

/// T-650: a snapshot without its `clock` member.
std::string withoutClock(std::string snap) {
    const size_t at = snap.find(",\"clock\":{");
    if (at != std::string::npos) {
        snap.erase(at, snap.find('}', at) + 1 - at);
    }
    return snap;
}

/// Load from inside the update hook: refused like hot_reload.
struct HookProbe {
    const char* json = nullptr;
    xt_status load = XT_STATUS_OK;
    int calls = 0;
    static void onUpdate(xt_surface* s, float /*dt*/, void* user) {
        auto* self = static_cast<HookProbe*>(user);
        ++self->calls;
        self->load = xt_surface_load_state(s, self->json);
    }
};

}  // namespace

TEST_CASE("capi state 1 (T-635): save, change ports / states / timeline / scroll, load: the same pixels") {
    StateSurface f;
    xt_surface* s = f.s;
    Frame rgba = newFrame();
    const std::string saved = setUpAndSave(s, rgba);
    const Frame atSave = rgba;

    const uint32_t hp = xt_surface_port_lookup(s, "hp");
    REQUIRE(xt_surface_port_set_number(s, hp, 80.0) == XT_STATUS_OK);
    REQUIRE(xt_surface_connection_pulse(s, "#Level.setState", "Ok") == XT_STATUS_OK);
    REQUIRE(xt_surface_seek_timeline(s, "", "pulse", 1.8f) == XT_STATUS_OK);
    wheel(s, -1.0f);  // Back to the top.
    updateAndRender(s, rgba, 0.1f);
    updateAndRender(s, rgba, 0.1f);
    CHECK(rgba != atSave);

    REQUIRE_MESSAGE(xt_surface_load_state(s, saved.c_str()) == XT_STATUS_OK, xt_capi_last_error());
    CHECK(xt_surface_port_get_number(s, hp) == doctest::Approx(25.0));
    updateAndRender(s, rgba);
    CHECK(rgba == atSave);  // Byte for byte.

    // A released timeline group comes back too: stop it, load again.
    REQUIRE(xt_surface_stop_timeline(s, "", "pulse") == XT_STATUS_OK);
    updateAndRender(s, rgba);
    CHECK(rgba != atSave);
    REQUIRE(xt_surface_load_state(s, saved.c_str()) == XT_STATUS_OK);
    updateAndRender(s, rgba);
    CHECK(rgba == atSave);
}

TEST_CASE("capi state 2 (T-635): a fresh surface restored before its first update renders the saved frame") {
    StateSurface f;
    xt_surface* s = f.s;
    Frame rgba = newFrame();
    const std::string saved = setUpAndSave(s, rgba);

    const xt_surface_desc desc = stateDesc();
    xt_surface* fresh = xt_surface_create(f.b, "state", &desc);
    REQUIRE_MESSAGE(fresh != nullptr, xt_capi_last_error());
    REQUIRE_MESSAGE(xt_surface_load_state(fresh, saved.c_str()) == XT_STATUS_OK, xt_capi_last_error());
    Frame first = newFrame();
    updateAndRender(fresh, first);  // Its first frame: full.
    CHECK(first == rgba);
    CHECK(xt_surface_is_timeline_playing(fresh, "", "pulse") == 1);
    const char* again = xt_surface_save_state(fresh);
    REQUIRE(again != nullptr);
    CHECK(withoutClock(again) == withoutClock(saved));  // T-650: one more update() counted.

    xt_surface_destroy(fresh);
}

TEST_CASE("capi state 3 (T-635): bad input is INVALID_ARGUMENT with nothing applied; NULL surface") {
    StateSurface f;
    xt_surface* s = f.s;
    Frame rgba = newFrame();
    const std::string saved = setUpAndSave(s, rgba);

    for (const char* bad : {static_cast<const char*>(nullptr), "", "{", "[]", "{\"version\":4}"}) {
        CAPTURE((bad != nullptr ? bad : "(NULL)"));
        CHECK(xt_surface_load_state(s, bad) == XT_STATUS_INVALID_ARGUMENT);
        CHECK(std::strlen(xt_capi_last_error()) > 0);
    }
    const char* now = xt_surface_save_state(s);
    REQUIRE(now != nullptr);
    CHECK(std::string(now) == saved);  // Nothing applied.

    CHECK(xt_surface_save_state(nullptr) == nullptr);
    CHECK(std::strlen(xt_capi_last_error()) > 0);
    CHECK(xt_surface_load_state(nullptr, saved.c_str()) == XT_STATUS_INVALID_ARGUMENT);
}

TEST_CASE("capi state 4 (T-635): load from inside the update hook is UNSUPPORTED") {
    StateSurface f;
    xt_surface* s = f.s;
    Frame rgba = newFrame();
    const std::string saved = setUpAndSave(s, rgba);

    HookProbe probe;
    probe.json = saved.c_str();
    REQUIRE(xt_surface_set_update_hook(s, &HookProbe::onUpdate, &probe) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    CHECK(probe.calls == 1);
    CHECK(probe.load == XT_STATUS_UNSUPPORTED);
    CHECK(std::string(xt_capi_last_error()).find("not from inside a callback") != std::string::npos);
    REQUIRE(xt_surface_set_update_hook(s, nullptr, nullptr) == XT_STATUS_OK);
    CHECK(xt_surface_load_state(s, saved.c_str()) == XT_STATUS_OK);  // Outside: fine.
}

TEST_CASE("capi state 5 (T-635): the saved string stays valid across other calls until the next save") {
    StateSurface f;
    xt_surface* s = f.s;
    Frame rgba = newFrame();
    updateAndRender(s, rgba);

    const char* first = xt_surface_save_state(s);
    REQUIRE(first != nullptr);
    const std::string copy = first;
    // Calls that hand out strings through the surface's scratch ring, more often than it has slots.
    const uint32_t hp = xt_surface_port_lookup(s, "hp");
    for (int i = 0; i < 20; ++i) {
        REQUIRE(xt_surface_port_set_number(s, hp, static_cast<double>(i)) == XT_STATUS_OK);
        CHECK(xt_surface_port_get_text(s, hp) != nullptr);
        CHECK(xt_surface_ui_id(s) != nullptr);
    }
    updateAndRender(s, rgba);
    CHECK(std::string(first) == copy);
    CHECK(xt_surface_load_state(s, first) == XT_STATUS_OK);

    // The next save replaces it (the pointer may or may not change; the old one is not used again).
    const char* second = xt_surface_save_state(s);
    REQUIRE(second != nullptr);
    CHECK(std::string(second) == copy);
}

// -- T-650 (23 XF-097 "version 2", 50 §50.y addendum): runtime classes, the clock, the first frame ---

namespace {

xt_surface_desc flowDesc(int width, int height, float dpr) {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = width;
    desc.height = height;
    desc.device_pixel_ratio = dpr;
    desc.premultiplied = 1;
    desc.run_flow = 1;
    return desc;
}

/// Counts the visibility callbacks of a surface.
struct VisibilityCount {
    int shown = 0;
    int hidden = 0;
    static void onVisibility(xt_surface* /*s*/, int visible, void* user) {
        auto* self = static_cast<VisibilityCount*>(user);
        ++(visible != 0 ? self->shown : self->hidden);
    }
};

/// A surface of `ui` (run_flow = 1) with a VisibilityCount listening; destroyed at scope end.
struct FlowSurface {
    xt_surface* s = nullptr;
    VisibilityCount visibility;
    FlowSurface(xt_bundle* b, const char* ui, const xt_surface_desc& desc) {
        s = xt_surface_create(b, ui, &desc);
        REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
        REQUIRE(xt_surface_set_visibility_listener(s, &VisibilityCount::onVisibility, &visibility) ==
                XT_STATUS_OK);
    }
    ~FlowSurface() { xt_surface_destroy(s); }
    FlowSurface(const FlowSurface&) = delete;
    FlowSurface& operator=(const FlowSurface&) = delete;
};

void renderInto(xt_surface* s, Frame& rgba, int width) {
    xt_damage damage{};
    REQUIRE(xt_surface_render(s, rgba.data(), width * 4, &damage) == XT_STATUS_OK);
}

void click(xt_surface* s, float x, float y) {
    for (const int32_t kind : {XT_INPUT_POINTER_MOVE, XT_INPUT_POINTER_DOWN, XT_INPUT_POINTER_UP}) {
        xt_input_event e{};
        e.struct_size = sizeof(e);
        e.kind = kind;
        e.x = x;
        e.y = y;
        e.gesture_scale = 1.0f;
        REQUIRE(xt_surface_push_input(s, &e) == XT_STATUS_OK);
    }
}

/// The first byte where two frames differ, as a message.
std::string firstDifference(const Frame& a, const Frame& b, int width) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) {
        ++i;
    }
    const size_t px = i / 4;
    return "pixel (" + std::to_string(px % static_cast<size_t>(width)) + ", " +
           std::to_string(px / static_cast<size_t>(width)) + ") channel " + std::to_string(i % 4);
}

}  // namespace

TEST_CASE("capi state 6 (T-650): a clicked card's class is back on a fresh surface's first frame") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_FLIP_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = flowDesc(kSize, kSize, 1.0f);
    std::string saved;
    Frame atSave = newFrame();
    {
        FlowSurface original(b, "flip", desc);
        REQUIRE(xt_surface_update(original.s, 0.0f) == XT_STATUS_OK);
        click(original.s, 32.0f, 32.0f);  // Flow: #card.Click -> toggleClass(flipped).
        REQUIRE(xt_surface_update(original.s, 0.0f) == XT_STATUS_OK);
        REQUIRE(xt_surface_update(original.s, 0.6f) == XT_STATUS_OK);
        REQUIRE(xt_surface_update(original.s, 0.6f) == XT_STATUS_OK);  // The 1 s opacity fade is over.
        renderInto(original.s, atSave, kSize);
        const char* json = xt_surface_save_state(original.s);
        REQUIRE_MESSAGE(json != nullptr, xt_capi_last_error());
        saved = json;
    }
    INFO(saved);
    CHECK(saved.find(R"("classes":{"card":{"add":["flipped"],"remove":[]}})") != std::string::npos);
    CHECK(saved.find(R"("shown":true})") != std::string::npos);

    {
        FlowSurface fresh(b, "flip", desc);
        REQUIRE_MESSAGE(xt_surface_load_state(fresh.s, saved.c_str()) == XT_STATUS_OK, xt_capi_last_error());
        Frame first = newFrame();
        REQUIRE(xt_surface_update(fresh.s, 0.0f) == XT_STATUS_OK);
        renderInto(fresh.s, first, kSize);
        CHECK_MESSAGE(first == atSave, firstDifference(first, atSave, kSize));  // Byte for byte.
        CHECK(fresh.visibility.shown == 0);  // The initial show was skipped: no visibility callback.

        // Without the snapshot the first frame is the unflipped red card.
        FlowSurface plain(b, "flip", desc);
        Frame red = newFrame();
        REQUIRE(xt_surface_update(plain.s, 0.0f) == XT_STATUS_OK);
        renderInto(plain.s, red, kSize);
        CHECK(red != atSave);
        CHECK(plain.visibility.shown == 1);
    }
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi state 7 (T-650): loaded before the first update, the intro is not replayed") {
    // fixtures/flip/intro: lifecycle.shown -> play(intro), a 0.5 s fade of the card from 0.25 to 1.
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_FLIP_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = flowDesc(kSize, kSize, 1.0f);
    {
        FlowSurface original(b, "intro", desc);
        Frame orig = newFrame();
        REQUIRE(xt_surface_update(original.s, 0.0f) == XT_STATUS_OK);
        REQUIRE(xt_surface_update(original.s, 0.2f) == XT_STATUS_OK);  // Mid-intro.
        renderInto(original.s, orig, kSize);
        REQUIRE(original.visibility.shown == 1);
        REQUIRE(xt_surface_is_timeline_playing(original.s, "", "intro") == 1);
        const char* json = xt_surface_save_state(original.s);
        REQUIRE_MESSAGE(json != nullptr, xt_capi_last_error());
        const std::string saved = json;
        INFO(saved);

        FlowSurface fresh(b, "intro", desc);
        REQUIRE_MESSAGE(xt_surface_load_state(fresh.s, saved.c_str()) == XT_STATUS_OK, xt_capi_last_error());
        Frame copy = newFrame();
        REQUIRE(xt_surface_update(fresh.s, 0.0f) ==
                XT_STATUS_OK);  // No silent update(0) needed before the load.
        renderInto(fresh.s, copy, kSize);
        CHECK_MESSAGE(copy == orig, firstDifference(copy, orig, kSize));  // The intro at 0.2 s, not at 0.
        CHECK(fresh.visibility.shown == 0);
        // The intro goes on from the snapshot, frame for frame with the original, and ends once.
        for (int f = 0; f < 4; ++f) {
            CAPTURE(f);
            REQUIRE(xt_surface_update(original.s, 0.125f) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(fresh.s, 0.125f) == XT_STATUS_OK);
            renderInto(original.s, orig, kSize);
            renderInto(fresh.s, copy, kSize);
            CHECK_MESSAGE(copy == orig, firstDifference(copy, orig, kSize));
        }
        CHECK(xt_surface_is_timeline_playing(fresh.s, "", "intro") == 0);
        CHECK(fresh.visibility.shown == 0);
        CHECK(original.visibility.shown == 1);
    }
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi state 8 (T-650): npdo main-menu loaded mid-rain: 60 frames equal the uninterrupted run") {
    // Debug renders at half size like test_capi_partial.cpp (same layout, device pixel ratio 0.5).
#ifdef NDEBUG
    constexpr int kW = 1280;
    constexpr int kH = 720;
    constexpr float kDpr = 1.0f;
#else
    constexpr int kW = 640;
    constexpr int kH = 360;
    constexpr float kDpr = 0.5f;
#endif
    constexpr float kDt = 1.0f / 60.0f;
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_NPDO_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = flowDesc(kW, kH, kDpr);
    const size_t bytes = static_cast<size_t>(kW) * kH * 4u;
    {
        FlowSurface original(b, "main-menu", desc);
        Frame orig(bytes, 0);
        for (int f = 0; f < 45; ++f) {  // lifecycle.shown plays intro, rain and fog (main-menu.xtflow).
            REQUIRE(xt_surface_update(original.s, f == 0 ? 0.0f : kDt) == XT_STATUS_OK);
            renderInto(original.s, orig, kW);
        }
        REQUIRE(xt_surface_is_timeline_playing(original.s, "", "rain") == 1);
        const char* json = xt_surface_save_state(original.s);
        REQUIRE_MESSAGE(json != nullptr, xt_capi_last_error());
        const std::string saved = json;
        INFO(saved);

        FlowSurface fresh(b, "main-menu", desc);
        REQUIRE_MESSAGE(xt_surface_load_state(fresh.s, saved.c_str()) == XT_STATUS_OK, xt_capi_last_error());
        Frame copy(bytes, 0);
        for (int f = 0; f < 60; ++f) {
            CAPTURE(f);
            REQUIRE(xt_surface_update(original.s, kDt) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(fresh.s, kDt) == XT_STATUS_OK);
            renderInto(original.s, orig, kW);
            renderInto(fresh.s, copy, kW);
            REQUIRE_MESSAGE(copy == orig, firstDifference(copy, orig, kW));
        }
        CHECK(xt_surface_is_timeline_playing(fresh.s, "", "rain") == 1);
        CHECK(fresh.visibility.shown == 0);
    }
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE(
    "capi state 9 (T-651): class-driven CSS animations loaded mid-run: 60 frames equal the uninterrupted "
    "run") {
    // fixtures/flip/lit: a click on #card toggles `lit`, which starts a 1.4 s infinite alternate
    // pulse, a 320 ms forwards one-shot and a 0.2 s infinite blink.
    constexpr float kDt = 1.0f / 60.0f;
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_FLIP_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = flowDesc(kSize, kSize, 1.0f);
    {
        FlowSurface original(b, "lit", desc);
        Frame orig = newFrame();
        for (int f = 0; f < 30; ++f) {
            if (f == 12) {
                click(original.s, 32.0f, 32.0f);
            }
            REQUIRE(xt_surface_update(original.s, f == 0 ? 0.0f : kDt) == XT_STATUS_OK);
            renderInto(original.s, orig, kSize);
        }
        const char* json = xt_surface_save_state(original.s);
        REQUIRE_MESSAGE(json != nullptr, xt_capi_last_error());
        const std::string saved = json;
        INFO(saved);
        CHECK(saved.find(R"("classes":{"card":{"add":["lit"],"remove":[]}})") != std::string::npos);
        CHECK(saved.find(R"({"id":"lamp","index":0,"name":"litpulse",)") != std::string::npos);
        CHECK(saved.find(R"({"id":"pop","index":0,"name":"popin",)") != std::string::npos);
        CHECK(saved.find(R"({"id":"blink","index":0,"name":"blinker",)") != std::string::npos);

        FlowSurface fresh(b, "lit", desc);
        REQUIRE_MESSAGE(xt_surface_load_state(fresh.s, saved.c_str()) == XT_STATUS_OK, xt_capi_last_error());
        Frame copy = newFrame();
        REQUIRE(xt_surface_update(fresh.s, 0.0f) == XT_STATUS_OK);
        renderInto(fresh.s, copy, kSize);
        CHECK_MESSAGE(copy == orig, firstDifference(copy, orig, kSize));  // The first frame: the saved one.
        for (int f = 0; f < 60; ++f) {
            CAPTURE(f);
            REQUIRE(xt_surface_update(original.s, kDt) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(fresh.s, kDt) == XT_STATUS_OK);
            renderInto(original.s, orig, kSize);
            renderInto(fresh.s, copy, kSize);
            REQUIRE_MESSAGE(copy == orig, firstDifference(copy, orig, kSize));
        }
        CHECK(fresh.visibility.shown == 0);

        // Without the snapshot the lamp is at rest: another frame.
        FlowSurface plain(b, "lit", desc);
        Frame rest = newFrame();
        REQUIRE(xt_surface_update(plain.s, 0.0f) == XT_STATUS_OK);
        renderInto(plain.s, rest, kSize);
        CHECK(rest != orig);
    }
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}
