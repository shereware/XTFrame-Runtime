// T-873 (Docs/design/60-input-channels.md §60.5): the input-channel functions of the C ABI --
// xt_input_channel (process-wide ids, malformed names), xt_surface_push_channel (the three kinds, the
// argument checks) and xt_surface_channel_value, plus the standard channels an xt_surface_push_input
// gamepad axis moves (the right stick, no longer dropped). XT_CAPI_TEST_BUNDLE is Examples/demos/game-ui.
#include <string>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

xt_surface_desc channelDesc() {
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = 320;
    desc.height = 200;
    desc.device_pixel_ratio = 1.0f;
    desc.premultiplied = 0;
    desc.run_flow = 0;
    desc.system_fonts = 0;
    desc.instance_id = 0;
    return desc;
}

}  // namespace

TEST_CASE("capi T-873: xt_input_channel ids, push_channel, channel_value") {
    const uint32_t rx = xt_input_channel("pad.rx");
    CHECK(rx != 0);
    CHECK(xt_input_channel("pad.rx") == rx);  // One name, one id.
    CHECK(xt_input_channel("midi.cc(07)") == xt_input_channel("midi.cc(7)"));
    CHECK(xt_input_channel("pad.nope") == 0);
    CHECK(std::string(xt_capi_last_error()).find("pad.nope") != std::string::npos);
    CHECK(xt_input_channel("player(1).pad.a") == 0);
    CHECK(xt_input_channel(nullptr) == 0);
    const uint32_t knob = xt_input_channel("host.knob");
    const uint32_t lever = xt_input_channel("host.lever");
    const uint32_t note = xt_input_channel("midi.note(36)");
    REQUIRE(knob != 0);
    REQUIRE(lever != 0);
    REQUIRE(note != 0);

    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    const xt_surface_desc desc = channelDesc();
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);

    CHECK(xt_surface_push_channel(s, knob, 2.0f, 1.0f, XT_CHANNEL_RELATIVE, 0) == XT_STATUS_OK);
    CHECK(xt_surface_push_channel(s, knob, 3.0f, 1.0f, XT_CHANNEL_RELATIVE, 0) == XT_STATUS_OK);
    CHECK(xt_surface_push_channel(s, lever, 0.25f, 1.0f, 0, 0) == XT_STATUS_OK);
    CHECK(xt_surface_push_channel(s, lever, 0.5f, 1.0f, 0, 2) == XT_STATUS_OK);
    CHECK(xt_surface_push_channel(s, note, 1.0f, 0.75f, 0, 0) == XT_STATUS_OK);
    // The values show only after the update.
    CHECK(xt_surface_channel_value(s, lever, 0) == 0.0f);
    int needs = 0;
    REQUIRE(xt_surface_needs_frame(s, &needs) == XT_STATUS_OK);
    CHECK(needs == 1);
    REQUIRE(xt_surface_update(s, 0.016f) == XT_STATUS_OK);
    CHECK(xt_surface_channel_value(s, knob, 0) == doctest::Approx(5.0f));
    CHECK(xt_surface_channel_value(s, lever, 0) == doctest::Approx(0.25f));
    CHECK(xt_surface_channel_value(s, lever, 2) == doctest::Approx(0.5f));
    CHECK(xt_surface_channel_value(s, note, 0) == 1.0f);
    REQUIRE(xt_surface_update(s, 0.016f) == XT_STATUS_OK);
    CHECK(xt_surface_channel_value(s, knob, 0) == 0.0f);  // A relative channel reads one update's sum.
    CHECK(xt_surface_channel_value(s, lever, 0) == doctest::Approx(0.25f));

    // The existing gamepad axis event moves pad.rx.
    xt_input_event e{};
    e.struct_size = sizeof(e);
    e.kind = XT_INPUT_GAMEPAD_AXIS_MOTION;
    e.gamepad_axis = 2;  // RightX.
    e.axis_value = -0.5f;
    e.gesture_scale = 1.0f;
    REQUIRE(xt_surface_push_input(s, &e) == XT_STATUS_OK);
    REQUIRE(xt_surface_update(s, 0.016f) == XT_STATUS_OK);
    CHECK(xt_surface_channel_value(s, rx, 0) == doctest::Approx(-0.5f));

    // Argument checks.
    CHECK(xt_surface_push_channel(nullptr, knob, 1.0f, 1.0f, 0, 0) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_push_channel(s, 0, 1.0f, 1.0f, 0, 0) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_push_channel(s, 0x7FFFFFFFu, 1.0f, 1.0f, 0, 0) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_push_channel(s, knob, 1.0f, 1.0f, 0, 8) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_channel_value(nullptr, knob, 0) == 0.0f);
    CHECK(xt_surface_channel_value(s, 0, 0) == 0.0f);
    CHECK(xt_surface_channel_value(s, knob, 9) == 0.0f);

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}
