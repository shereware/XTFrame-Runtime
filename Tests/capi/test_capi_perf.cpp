// T-750 (Docs/design/57-measurement.md §57.2, §57.5): xt_frame_record and its CSV form through the C
// header only -- the record after a frame, a caller struct smaller than the library's (only the front
// part written), the column table's header and rows. T-749: xt_surface_needs_frame and
// xt_surface_set_perf_detail.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

size_t columns(const std::string& line) {
    size_t n = 1;
    for (char c : line) {
        n += c == ',' ? 1u : 0u;
    }
    return n;
}

std::string header() {
    std::string text(xt_frame_record_csv_header(nullptr, 0), '\0');
    xt_frame_record_csv_header(text.data(), text.size() + 1);
    return text;
}

std::string row(const xt_frame_record* r) {
    std::string text(xt_frame_record_csv_row(r, nullptr, 0), '\0');
    xt_frame_record_csv_row(r, text.data(), text.size() + 1);
    return text;
}

}  // namespace

TEST_CASE("capi perf 1: xt_frame_record after a frame, its CSV row and a smaller caller struct") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = 320;
    desc.height = 200;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 1;
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    xt_surface_set_perf_enabled(s, 1);
    std::vector<uint8_t> rgba(320u * 200u * 4u);
    xt_damage damage{};
    for (int i = 0; i < 2; ++i) {
        REQUIRE(xt_surface_update(s, 1.0f / 60.0f) == XT_STATUS_OK);
        REQUIRE(xt_surface_render(s, rgba.data(), 320 * 4, &damage) == XT_STATUS_OK);
    }

    xt_frame_record rec;
    std::memset(&rec, 0, sizeof(rec));
    rec.struct_size = sizeof(rec);
    REQUIRE(xt_surface_frame_record(s, &rec) == XT_STATUS_OK);
    CHECK(rec.struct_size == sizeof(rec));
    CHECK(rec.version == 1u);
    CHECK(rec.frame >= 1u);
    CHECK(rec.backend == 1u);  // the surface's SW renderer
    CHECK(rec.build_ms > 0.0f);
    CHECK(rec.commands > 0u);
    CHECK(rec.arena_nodes > 0u);
    CHECK(rec.frame_marked == 0u);  // the C ABI does not mark frames
    CHECK(rec.unaccounted_ms == 0.0f);
    xt_frame_stats st{};
    REQUIRE(xt_surface_frame_stats(s, &st) == XT_STATUS_OK);
    CHECK(rec.build_ms == st.render_ms);  // the SW render is the build phase

    const std::string h = header();
    const std::string r = row(&rec);
    CHECK(columns(h) == columns(r));
    CHECK(h.rfind("version,frame,steady,time_s,", 0) == 0);
    CHECK(h.find(",unaccounted_ms,") != std::string::npos);
    CHECK(h.find(",cause_binding_layout,") != std::string::npos);
    CHECK(r.rfind("1," + std::to_string(rec.frame) + ",0,", 0) == 0);

    // A caller built against a shorter struct gets only the fields its struct_size covers.
    const uint32_t shortSize = static_cast<uint32_t>(offsetof(xt_frame_record, frame_wall_ms));
    xt_frame_record part;
    std::memset(&part, 0xAB, sizeof(part));
    part.struct_size = shortSize;
    REQUIRE(xt_surface_frame_record(s, &part) == XT_STATUS_OK);
    CHECK(part.version == 1u);
    CHECK(part.frame == rec.frame);
    CHECK(part.backend == rec.backend);
    uint32_t untouched = 0;
    std::memcpy(&untouched, &part.frame_wall_ms, sizeof(untouched));
    CHECK(untouched == 0xABABABABu);
    // ... and its CSV row writes the rest as 0, with as many columns as the header.
    const std::string pr = row(&part);
    CHECK(columns(pr) == columns(h));

    xt_frame_record tiny{};
    tiny.struct_size = 4;
    CHECK(xt_surface_frame_record(s, &tiny) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_frame_record(nullptr, &rec) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_frame_record(s, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_frame_record_csv_row(nullptr, nullptr, 0) == 0u);
    char small[8];
    CHECK(xt_frame_record_csv_header(small, sizeof(small)) == h.size());
    CHECK(std::string(small) == h.substr(0, 7));

    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi perf 2: marked frames close the budget, thread CPU timing fills the cpu columns") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = 320;
    desc.height = 200;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 1;
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    xt_surface_set_perf_enabled(s, 1);
    REQUIRE(xt_surface_set_thread_cpu_timing(s, 1) == XT_STATUS_OK);
    std::vector<uint8_t> rgba(320u * 200u * 4u);
    xt_damage damage{};
    xt_frame_record rec;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(xt_surface_mark_frame_start(s) == XT_STATUS_OK);
        REQUIRE(xt_surface_update(s, 1.0f / 60.0f) == XT_STATUS_OK);
        REQUIRE(xt_surface_render(s, rgba.data(), 320 * 4, &damage) == XT_STATUS_OK);
        REQUIRE(xt_surface_mark_frame_end(s) == XT_STATUS_OK);
        std::memset(&rec, 0, sizeof(rec));
        rec.struct_size = sizeof(rec);
        REQUIRE(xt_surface_frame_record(s, &rec) == XT_STATUS_OK);
        CHECK(rec.frame_marked == 1u);
        CHECK(rec.frame_wall_ms > 0.0f);
        CHECK(rec.frame_cpu_ms > 0.0f);
        // The budget closes: the phases plus the remainder are the frame; the clear is in build.
        const double sum = double(rec.input_ms) + rec.update_ms + rec.flow_ms + rec.paint_ms + rec.damage_ms +
                           rec.build_ms + rec.submit_ms + rec.wait_ms + rec.unaccounted_ms;
        CHECK(sum == doctest::Approx(double(rec.frame_wall_ms)).epsilon(1e-3));
        CHECK(rec.unaccounted_ms < rec.frame_wall_ms);
    }
    CHECK(rec.redundant_styled == 0u);  // the C ABI leaves the detail level at 0
    // An unmarked frame after them carries no frame time.
    REQUIRE(xt_surface_update(s, 1.0f / 60.0f) == XT_STATUS_OK);
    REQUIRE(xt_surface_render(s, rgba.data(), 320 * 4, &damage) == XT_STATUS_OK);
    std::memset(&rec, 0, sizeof(rec));
    rec.struct_size = sizeof(rec);
    REQUIRE(xt_surface_frame_record(s, &rec) == XT_STATUS_OK);
    CHECK(rec.frame_marked == 0u);
    CHECK(rec.unaccounted_ms == 0.0f);
    CHECK(xt_surface_mark_frame_start(nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_mark_frame_end(nullptr) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_set_thread_cpu_timing(nullptr, 1) == XT_STATUS_INVALID_ARGUMENT);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

namespace {

xt_input_event perfPointer(int32_t kind, float x, float y) {
    xt_input_event e{};
    e.struct_size = sizeof(e);
    e.kind = kind;
    e.x = x;
    e.y = y;
    e.gesture_scale = 1.0f;
    return e;
}

int needsFrame(const xt_surface* s) {
    int out = -1;
    REQUIRE(xt_surface_needs_frame(s, &out) == XT_STATUS_OK);
    REQUIRE((out == 0 || out == 1));
    return out;
}

}  // namespace

TEST_CASE("capi perf 3 (T-749): xt_surface_needs_frame over input, a flow edge and a transition") {
    // The flip fixture: a click on #card toggles `flipped` through its .xtflow, which starts a 1 s
    // opacity transition.
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_FLIP_BUNDLE);
    REQUIRE_MESSAGE(b != nullptr, xt_capi_last_error());
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = 64;
    desc.height = 64;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 1;
    xt_surface* s = xt_surface_create(b, "flip", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    std::vector<uint8_t> rgba(64u * 64u * 4u);
    xt_damage damage{};
    auto frame = [&](float dt) {
        REQUIRE(xt_surface_update(s, dt) == XT_STATUS_OK);
        REQUIRE(xt_surface_render(s, rgba.data(), 64 * 4, &damage) == XT_STATUS_OK);
    };
    auto settle = [&](int limit) {
        for (int i = 0; i <= limit; ++i) {
            if (needsFrame(s) == 0) {
                return i;
            }
            frame(0.1f);
        }
        return -1;
    };
    CHECK(needsFrame(s) == 1);  // Never updated or rendered.
    CHECK(settle(3) > 0);
    const std::vector<uint8_t> idle = rgba;
    // Idle: an update at a later clock and a render change nothing (no damage, same pixels).
    frame(3.0f);
    CHECK(needsFrame(s) == 0);
    CHECK(damage.full == 0);
    CHECK(damage.changed == 0);
    CHECK(damage.rect_count == 0u);
    CHECK(rgba == idle);

    // A pointer move alone wakes it; it settles again.
    const xt_input_event move = perfPointer(XT_INPUT_POINTER_MOVE, 32.0f, 32.0f);
    REQUIRE(xt_surface_push_input(s, &move) == XT_STATUS_OK);
    CHECK(needsFrame(s) == 1);
    CHECK(settle(3) > 0);

    // A click: the flow toggles the class, the 1 s transition keeps it awake until it ends.
    const xt_input_event down = perfPointer(XT_INPUT_POINTER_DOWN, 32.0f, 32.0f);
    const xt_input_event up = perfPointer(XT_INPUT_POINTER_UP, 32.0f, 32.0f);
    REQUIRE(xt_surface_push_input(s, &down) == XT_STATUS_OK);
    REQUIRE(xt_surface_push_input(s, &up) == XT_STATUS_OK);
    CHECK(needsFrame(s) == 1);
    for (int i = 0; i < 8; ++i) {  // 0.8 s of the 1 s
        frame(0.1f);
        CHECK(needsFrame(s) == 1);
    }
    CHECK(settle(6) > 0);

    // The perf detail level: 0..2, anything else refused.
    xt_surface_set_perf_enabled(s, 1);
    CHECK(xt_surface_set_perf_detail(s, 1) == XT_STATUS_OK);
    CHECK(xt_surface_set_perf_detail(s, 2) == XT_STATUS_OK);
    CHECK(xt_surface_set_perf_detail(s, 0) == XT_STATUS_OK);
    CHECK(xt_surface_set_perf_detail(s, 3) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_set_perf_detail(s, -1) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(xt_surface_set_perf_detail(nullptr, 1) == XT_STATUS_INVALID_ARGUMENT);
    int out = -1;
    CHECK(xt_surface_needs_frame(nullptr, &out) == XT_STATUS_INVALID_ARGUMENT);
    CHECK(out == -1);
    CHECK(xt_surface_needs_frame(s, nullptr) == XT_STATUS_INVALID_ARGUMENT);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}

TEST_CASE("capi perf 4 (T-749): perf detail 1 fills the redundant-work columns, 0 leaves them 0") {
    xt_bundle* b = xt_bundle_open(XT_CAPI_TEST_BUNDLE);
    REQUIRE(b != nullptr);
    xt_surface_desc desc{};
    desc.struct_size = sizeof(desc);
    desc.width = 320;
    desc.height = 200;
    desc.device_pixel_ratio = 1.0f;
    desc.run_flow = 1;
    xt_surface* s = xt_surface_create(b, "hud", &desc);
    REQUIRE_MESSAGE(s != nullptr, xt_capi_last_error());
    xt_surface_set_perf_enabled(s, 1);
    std::vector<uint8_t> rgba(320u * 200u * 4u);
    xt_damage damage{};
    uint32_t redundant[2] = {0, 0};
    for (int level = 0; level < 2; ++level) {
        REQUIRE(xt_surface_set_perf_detail(s, level) == XT_STATUS_OK);
        for (int i = 0; i < 3; ++i) {
            // A resize every frame: everything is laid out and painted again, mostly unchanged.
            REQUIRE(xt_surface_resize(s, 320, 200 - (i % 2), 1.0f) == XT_STATUS_OK);
            REQUIRE(xt_surface_update(s, 1.0f / 60.0f) == XT_STATUS_OK);
            REQUIRE(xt_surface_render(s, rgba.data(), 320 * 4, &damage) == XT_STATUS_OK);
            xt_frame_record rec;
            std::memset(&rec, 0, sizeof(rec));
            rec.struct_size = sizeof(rec);
            REQUIRE(xt_surface_frame_record(s, &rec) == XT_STATUS_OK);
            redundant[level] += rec.redundant_commands + rec.redundant_styled + rec.redundant_laid_out;
        }
    }
    CHECK(redundant[0] == 0u);
    CHECK(redundant[1] > 0u);
    xt_surface_destroy(s);
    CHECK(xt_bundle_close(b) == XT_STATUS_OK);
}
