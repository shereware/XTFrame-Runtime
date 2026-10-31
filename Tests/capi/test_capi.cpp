// T-504 (Docs/design/50-c-abi.md §50.4): the C ABI shared library xtframe, through its C header
// only. XT_CAPI_TEST_BUNDLE is Examples/demos/game-ui and XT_CAPI_TEST_PORTS_BUNDLE
// Examples/demos/hud-overlay, both packed by xtframe-pack at build time (Tests/capi/CMakeLists.txt).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"
#include "xt/version.h"  // XT_VERSION_* macros only (case 1).

namespace {

xt_surface_desc makeDesc(int width, int height) {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = width;
    desc.height = height;
    desc.device_pixel_ratio = 1.0f;
    desc.premultiplied = 0;
    desc.run_flow = 1;
    desc.system_fonts = 0;
    desc.instance_id = 0;
    return desc;
}

xt_input_event pointer(int32_t kind, float x, float y) {
    xt_input_event e{};
    e.struct_size = sizeof(e);
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.gesture_scale = 1.0f;
    return e;
}

/// RGBA8 buffer of a surface, kept across frames (partial redraw).
struct Pixels {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    Pixels(int w, int h)
        : width(w), height(h), rgba(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0) {}
    xt_status render(xt_surface* s, xt_damage& damage) {
        return xt_surface_render(s, rgba.data(), width * 4, &damage);
    }
    bool anyNonZero() const {
        for (uint8_t v : rgba) {
            if (v != 0) {
                return true;
            }
        }
        return false;
    }
};

/// Every port event a surface reported.
struct PortLog {
    std::vector<xt_port_event> events;
    std::vector<std::string> names;
    std::vector<std::string> texts;
    static void callback(xt_surface* /*s*/, const xt_port_event* e, void* user) {
        auto* self = static_cast<PortLog*>(user);
        self->events.push_back(*e);
        self->names.emplace_back(e->name != nullptr ? e->name : "");
        self->texts.emplace_back(e->text != nullptr ? e->text : "");
    }
    size_t count(int kind, uint32_t listener) const {
        size_t n = 0;
        for (const xt_port_event& e : events) {
            if (e.kind == kind && e.listener_id == listener) {
                ++n;
            }
        }
        return n;
    }
};

std::string readText(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("capi 1: ABI version and version string") {
    CHECK(xt_capi_abi_version() == XT_CAPI_ABI_VERSION);
    char expected[32];
    std::snprintf(expected, sizeof(expected), "%d.%d.%d", XT_VERSION_MAJOR, XT_VERSION_MINOR,
                  XT_VERSION_PATCH);
    CHECK(std::string(xt_capi_version_string()) == expected);
}

TEST_CASE("capi 2: bundle open, ui count, ui info, find_ui") {
    CHECK(xt_bundle_open(XT_CAPI_TEST_BUNDLE "/does-not-exist") == nullptr);
    CHECK(std::strlen(xt_capi_last_error()) > 0);

    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    const uint32_t count = xt_bundle_ui_count(b);
    CHECK(count > 0);

    const nlohmann::json manifest = nlohmann::json::parse(readText(XT_CAPI_TEST_BUNDLE "/manifest.json"));
    REQUIRE(manifest["uis"].size() == count);
    for (uint32_t i = 0; i < count; ++i) {
        xt_ui_info info{};
        REQUIRE(xt_bundle_ui_info(b, i, &info) == XT_STATUS_OK);
        const nlohmann::json& ui = manifest["uis"][i];
        CHECK(std::string(info.id) == ui["id"].get<std::string>());
        CHECK(info.design_width == doctest::Approx(ui["size"][0].get<double>()));
        CHECK(info.design_height == doctest::Approx(ui["size"][1].get<double>()));
        CHECK(xt_bundle_find_ui(b, ui["id"].get<std::string>().c_str()) == static_cast<int32_t>(i));
    }
    xt_ui_info out{};
    CHECK(xt_bundle_ui_info(b, count, &out) == XT_STATUS_OUT_OF_RANGE);
    CHECK(xt_bundle_find_ui(b, "no-such-ui") == -1);
    CHECK(std::string(xt_bundle_id(b)) == manifest["bundle"]["id"].get<std::string>());
    xt_bundle_spec spec{};
    CHECK(xt_bundle_get_spec(b, &spec) == XT_STATUS_OK);
    CHECK(spec.logical_height == doctest::Approx(720.0));
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 3 + 4: first frame full, static frame unchanged, resize full + uiscale") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    const xt_surface_desc desc = makeDesc(320, 200);
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    CHECK(std::string(xt_surface_ui_id(s)) == "hud");

    Pixels pixels(320, 200);
    xt_damage damage{};
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(pixels.render(s, damage) == XT_STATUS_OK);
    CHECK(damage.full == 1);
    CHECK(damage.changed == 1);
    CHECK(pixels.anyNonZero());

    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(pixels.render(s, damage) == XT_STATUS_OK);
    CHECK(damage.full == 0);
    CHECK(damage.changed == 0);
    CHECK(damage.rect_count == 0);

    // Case 4: spec.uiscaleRule height, logicalHeight 720.
    const float before = xt_surface_ui_scale(s);
    CHECK(before == doctest::Approx(200.0 / 720.0));
    REQUIRE(xt_surface_resize(s, 640, 400, 1.0f) == XT_STATUS_OK);
    CHECK(xt_surface_ui_scale(s) == doctest::Approx(400.0 / 720.0));
    Pixels bigger(640, 400);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(bigger.render(s, damage) == XT_STATUS_OK);
    CHECK(damage.full == 1);
    CHECK(damage.changed == 1);
    // An override wins until it is cleared.
    REQUIRE(xt_surface_set_ui_scale(s, 2.0f) == XT_STATUS_OK);
    CHECK(xt_surface_ui_scale(s) == doctest::Approx(2.0));
    REQUIRE(xt_surface_set_ui_scale(s, 0.0f) == XT_STATUS_OK);
    CHECK(xt_surface_ui_scale(s) == doctest::Approx(400.0 / 720.0));
    // Argument checks of render.
    CHECK(xt_surface_render(s, nullptr, 640 * 4, &damage) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_render(s, bigger.rgba.data(), 640 * 4 - 1, &damage) == XT_STATUS_INVALID_ARGUMENT);

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 5: a click on #fire reaches connection_watch inside update") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    // The design size: uiscale 1, logical = device pixels.
    const xt_surface_desc desc = makeDesc(1280, 720);
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    PortLog log;
    REQUIRE(xt_surface_set_port_listener(s, &PortLog::callback, &log) == XT_STATUS_OK);
    uint32_t enter = 0;
    uint32_t leave = 0;
    uint32_t click = 0;
    REQUIRE(xt_surface_connection_watch(s, "#fire.PointerEnter", &enter) == XT_STATUS_OK);
    REQUIRE(xt_surface_connection_watch(s, "#fire.PointerLeave", &leave) == XT_STATUS_OK);
    REQUIRE(xt_surface_connection_watch(s, "#fire.Click", &click) == XT_STATUS_OK);
    CHECK(enter != click);
    uint32_t bad = 0;
    CHECK(xt_surface_connection_watch(s, "#nope.Click", &bad) == XT_STATUS_INVALID_ARGUMENT);
    Pixels pixels(1280, 720);
    xt_damage damage{};
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(pixels.render(s, damage) == XT_STATUS_OK);

    // Find a point on the button without knowing the layout: sweep the rows (one update per row)
    // until #fire.PointerEnter fires, then the points of that row one by one.
    const float step = 8.0f;
    bool found = false;
    float hitX = 0;
    float hitY = 0;
    for (float y = step / 2; y < 720.0f && !found; y += step) {
        const size_t before = log.count(XT_PORT_CONNECTION, enter);
        for (float x = step / 2; x < 1280.0f; x += step) {
            xt_input_event move = pointer(XT_INPUT_POINTER_MOVE, x, y);
            REQUIRE(xt_surface_push_input(s, &move) == XT_STATUS_OK);
        }
        REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
        if (log.count(XT_PORT_CONNECTION, enter) == before) {
            continue;
        }
        for (float x = step / 2; x < 1280.0f && !found; x += step) {
            xt_input_event away = pointer(XT_INPUT_POINTER_MOVE, 1.0f, 1.0f);
            xt_input_event move = pointer(XT_INPUT_POINTER_MOVE, x, y);
            REQUIRE(xt_surface_push_input(s, &away) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
            const size_t entered = log.count(XT_PORT_CONNECTION, enter);
            REQUIRE(xt_surface_push_input(s, &move) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
            if (log.count(XT_PORT_CONNECTION, enter) == entered) {
                continue;
            }
            // T-552 (27 §27.4.1): this frame's hover was judged against the geometry before its
            // bindings -- the HUD's panels tilt with the pointer (`on:pointermove` + `bind:rotate-*`,
            // T-542) -- and the next update() judges it again against the tilted panel before
            // anything else, with no further input. A point #fire is still under then (no
            // PointerLeave) is under it for the click as well.
            const size_t left = log.count(XT_PORT_CONNECTION, leave);
            REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
            if (log.count(XT_PORT_CONNECTION, leave) == left) {
                found = true;
                hitX = x;
                hitY = y;
            }
        }
    }
    REQUIRE_MESSAGE(found, "no point of the viewport is on #fire");

    xt_input_event down = pointer(XT_INPUT_POINTER_DOWN, hitX, hitY);
    xt_input_event up = pointer(XT_INPUT_POINTER_UP, hitX, hitY);
    REQUIRE(xt_surface_push_input(s, &down) == XT_STATUS_OK);
    REQUIRE(xt_surface_push_input(s, &up) == XT_STATUS_OK);
    CHECK(log.count(XT_PORT_CONNECTION, click) == 0);  // Queued only; dispatched by update.
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    REQUIRE(log.count(XT_PORT_CONNECTION, click) == 1);
    for (size_t i = 0; i < log.events.size(); ++i) {
        if (log.events[i].kind == XT_PORT_CONNECTION && log.events[i].listener_id == click) {
            CHECK(log.names[i] == "#fire.Click");
            CHECK(log.events[i].port == 0u);
            CHECK(log.events[i].value_type == 0);  // PortType pulse.
        }
    }
    xt_surface_connection_unwatch(s, click);
    REQUIRE(xt_surface_push_input(s, &down) == XT_STATUS_OK);
    REQUIRE(xt_surface_push_input(s, &up) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    CHECK(log.count(XT_PORT_CONNECTION, click) == 1);

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 6: declared ports") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_PORTS_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = makeDesc(480, 270);
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());

    CHECK(xt_surface_port_count(s) > 0);
    const uint32_t hp = xt_surface_port_lookup(s, "hp");
    REQUIRE(hp != XT_INVALID_PORT);
    bool listed = false;
    for (uint32_t i = 0; i < xt_surface_port_count(s); ++i) {
        xt_port_record row{};
        REQUIRE(xt_surface_port_record(s, i, &row) == XT_STATUS_OK);
        if (row.id == hp) {
            listed = true;
            CHECK(std::string(row.name) == "hp");
            CHECK(row.value_type == 1);  // PortValueType::Number.
            CHECK(row.dir == 0);         // in.
            CHECK(row.document_level == 1);
        }
    }
    CHECK(listed);
    REQUIRE(xt_surface_port_set_number(s, hp, 42.0) == XT_STATUS_OK);
    CHECK(xt_surface_port_get_number(s, hp) == doctest::Approx(42.0));
    CHECK(std::string(xt_surface_port_get_text(s, hp)) == "42");
    REQUIRE(xt_surface_port_set_number(s, hp, 12.5) == XT_STATUS_OK);
    CHECK(std::string(xt_surface_port_get_text(s, hp)) == "12.5");
    REQUIRE(xt_surface_port_set_text(s, hp, "7") == XT_STATUS_OK);
    CHECK(xt_surface_port_get_number(s, hp) == doctest::Approx(7.0));

    CHECK(xt_surface_port_lookup(s, "no-such-port") == XT_INVALID_PORT);
    CHECK(xt_surface_port_set_number(s, 9999u, 1.0) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_port_set_number(s, XT_INVALID_PORT, 1.0) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_port_pulse(s, hp) == XT_STATUS_INVALID_ARGUMENT);  // Not an event port.
    const uint32_t damage = xt_surface_port_lookup(s, "damage");
    REQUIRE(damage != XT_INVALID_PORT);
    CHECK(xt_surface_port_pulse(s, damage) == XT_STATUS_OK);

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 7: TEXT_INPUT is split per code point; invalid UTF-8 is refused") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    const xt_surface_desc desc = makeDesc(320, 200);
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE(s != nullptr);

    const uint32_t start = xt_debug_surface_input_count(s);
    const char text[] = "ab\xE2\x82\xAC";  // "ab€": 3 code points, 5 bytes.
    xt_input_event e = pointer(XT_INPUT_TEXT_INPUT, 0, 0);
    e.text = text;
    e.text_len = 5;
    REQUIRE(xt_surface_push_input(s, &e) == XT_STATUS_OK);
    CHECK(xt_debug_surface_input_count(s) == start + 3);

    const char* invalid[] = {"\xC3\x28", "\xE2\x82", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x80"};
    for (const char* bytes : invalid) {
        e.text = bytes;
        e.text_len = static_cast<uint32_t>(std::strlen(bytes));
        CHECK(xt_surface_push_input(s, &e) == XT_STATUS_INVALID_ARGUMENT);
    }
    CHECK(xt_debug_surface_input_count(s) == start + 3);

    xt_input_event wrong = pointer(16, 0, 0);
    CHECK(xt_surface_push_input(s, &wrong) == XT_STATUS_INVALID_ARGUMENT);
    xt_input_event move = pointer(XT_INPUT_POINTER_MOVE, 5, 5);
    REQUIRE(xt_surface_push_input(s, &move) == XT_STATUS_OK);
    CHECK(xt_debug_surface_input_count(s) == start + 4);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 8: bundle_close refuses while a surface lives") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    const xt_surface_desc desc = makeDesc(320, 200);
    xt_surface* s = xt_surface_create(b, "pause", &desc);
    REQUIRE(s != nullptr);
    CHECK(xt_bundle_close(b) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(std::strlen(xt_capi_last_error()) > 0);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

namespace {

struct ReentryProbe {
    xt_bundle* bundle = nullptr;
    int calls = 0;
    xt_status hotReload = XT_STATUS_OK;
    xt_status bundleClose = XT_STATUS_OK;
    xt_status update = XT_STATUS_INTERNAL;
    static void callback(xt_surface* s, const xt_port_event* /*e*/, void* user) {
        auto* self = static_cast<ReentryProbe*>(user);
        if (self->calls++ > 0) {
            return;
        }
        self->hotReload = xt_surface_hot_reload(s);
        self->bundleClose = xt_bundle_close(self->bundle);
        xt_surface_destroy(s);                            // Refused: the surface must survive.
        self->update = xt_surface_set_ui_scale(s, 0.0f);  // Other calls are allowed.
    }
};

}  // namespace

TEST_CASE("capi 9: destroy / hot_reload / bundle_close inside a callback") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    const xt_surface_desc desc = makeDesc(320, 200);
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE(s != nullptr);
    ReentryProbe probe;
    probe.bundle = b;
    REQUIRE(xt_surface_set_port_listener(s, &ReentryProbe::callback, &probe) == XT_STATUS_OK);
    uint32_t clock = 0;
    REQUIRE(xt_surface_connection_watch(s, "document.clock", &clock) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(s, 0.1f) == XT_STATUS_OK);
    REQUIRE(probe.calls > 0);
    CHECK(probe.hotReload == XT_STATUS_UNSUPPORTED);
    CHECK(probe.bundleClose == XT_STATUS_UNSUPPORTED);
    CHECK(probe.update == XT_STATUS_OK);
    // Still alive; outside the callback hot reload works (item 8 of the task sheet), and the
    // connection watch survives it.
    CHECK(std::string(xt_surface_ui_id(s)) == "hud");
    REQUIRE(xt_surface_set_port_listener(s, nullptr, nullptr) == XT_STATUS_OK);
    CHECK(xt_surface_hot_reload(s) == XT_STATUS_OK);
    PortLog log;
    REQUIRE(xt_surface_set_port_listener(s, &PortLog::callback, &log) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(s, 0.1f) == XT_STATUS_OK);
    CHECK(log.count(XT_PORT_CONNECTION, clock) > 0);
    Pixels pixels(320, 200);
    xt_damage damage{};
    REQUIRE(pixels.render(s, damage) == XT_STATUS_OK);
    CHECK(damage.full == 1);

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 10 (T-552, review K1): reducedMotion survives resize, set_safe_area and set_input_kind") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_DEVICE_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = makeDesc(64, 64);
    xt_surface* s = xt_surface_create(b, "device", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    // The box is red, and blue while `@media (prefers-reduced-motion: reduce)` matches.
    auto blue = [&](int size) {
        Pixels pixels(size, size);
        xt_damage damage{};
        REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
        REQUIRE(pixels.render(s, damage) == XT_STATUS_OK);
        const size_t center =
            (static_cast<size_t>(size / 2) * static_cast<size_t>(size) + static_cast<size_t>(size / 2)) * 4u;
        INFO("rgba " << int(pixels.rgba[center]) << " " << int(pixels.rgba[center + 1]) << " "
                     << int(pixels.rgba[center + 2]) << " " << int(pixels.rgba[center + 3]));
        REQUIRE(pixels.rgba[center + 3] > 200);
        return pixels.rgba[center + 2] > 200 && pixels.rgba[center] < 50;
    };
    CHECK_FALSE(blue(64));
    REQUIRE(xt_surface_post_system_event(s, "reducedMotion", "reduce") == XT_STATUS_OK);
    CHECK(blue(64));
    // Each of these used to set the surface's own DeviceInfo copy, without reducedMotion.
    REQUIRE(xt_surface_resize(s, 48, 48, 1.0f) == XT_STATUS_OK);
    CHECK(blue(48));
    REQUIRE(xt_surface_set_safe_area(s, 0.0f, 0.0f, 0.0f, 0.0f) == XT_STATUS_OK);
    CHECK(blue(48));
    REQUIRE(xt_surface_set_input_kind(s, 2) == XT_STATUS_OK);
    CHECK(blue(48));
    REQUIRE(xt_surface_resize(s, 64, 64, 1.0f) == XT_STATUS_OK);
    CHECK(blue(64));
    REQUIRE(xt_surface_post_system_event(s, "reducedMotion", "no-preference") == XT_STATUS_OK);
    CHECK_FALSE(blue(64));
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi 11 (T-552, review K4): the stride check is done in size_t; a row past an int is refused") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_DEVICE_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    const xt_surface_desc desc = makeDesc(64, 64);
    xt_surface* s = xt_surface_create(b, "device", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    std::vector<uint8_t> small(64u * 64u * 4u, 0);
    xt_damage damage{};
    CHECK(xt_surface_render(s, small.data(), -1, &damage) == XT_STATUS_INVALID_ARGUMENT);
    // 2^30 * 4 wraps to 0 in a 32-bit int: the old check let any stride through and the renderer
    // wrote rows of 4 GiB into a 16 KiB buffer. The buffer is never touched now.
    REQUIRE(xt_surface_resize(s, 1 << 30, 64, 1.0f) == XT_STATUS_OK);
    CHECK(xt_surface_render(s, small.data(), 64 * 4, &damage) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_render(s, small.data(), 2147483647, &damage) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(std::string(xt_capi_last_error()).find("stride_bytes") != std::string::npos);
    // INT_MAX / 4 + 1 pixels: the row (2^31 bytes) does not fit in an int stride.
    REQUIRE(xt_surface_resize(s, 536870912, 64, 1.0f) == XT_STATUS_OK);
    CHECK(xt_surface_render(s, small.data(), 2147483647, &damage) == XT_STATUS_INVALID_ARGUMENT);
    // Back to a real size: the same buffer renders.
    REQUIRE(xt_surface_resize(s, 64, 64, 1.0f) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
    CHECK(xt_surface_render(s, small.data(), 64 * 4, &damage) == XT_STATUS_OK);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}
