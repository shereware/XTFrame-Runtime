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
    CHECK(std::string(again) == saved);

    xt_surface_destroy(fresh);
}

TEST_CASE("capi state 3 (T-635): bad input is INVALID_ARGUMENT with nothing applied; NULL surface") {
    StateSurface f;
    xt_surface* s = f.s;
    Frame rgba = newFrame();
    const std::string saved = setUpAndSave(s, rgba);

    for (const char* bad : {static_cast<const char*>(nullptr), "", "{", "[]", "{\"version\":2}"}) {
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
