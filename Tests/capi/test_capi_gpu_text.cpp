// T-647 (Docs/design/50-c-abi.md §50.z addendum): text on the C ABI GPU target against
// xt_surface_render, and xt_gpu_target_desc.sample_count. Fixture Tests/capi/fixtures/text (480x400,
// uiscale 1, bundled Noto Sans): 12 / 24 / 48 px light text on an opaque dark band, the same sizes in
// dark text on transparent, a non-convex star path and a 120 px word (outlines on both backends).
// The whole frame is held to 10 §10.10's GPU / SW tolerance (a channel differing by more than 16
// marks the pixel; at most 3 % of the pixels) at sample_count 1 and 4, and so are the star and the
// word on their own; the 12 px lines, which both paths draw from the same hinted bitmaps with the
// same gamma since T-647, to a channel difference of 2. The 24 / 48 px lines are only reported: they
// are the documented 24-96 px band (10 §10.x T-263c: SW outlines, GPU light-hinted atlas bitmaps).
// T-653: the bundle's second UI, `skew`, holds a note with a 2 px left border, 12 / 24 px text and a
// 1 px box, axis-aligned, under skewX(-6deg) and under rotate(3deg) (case 4).
// Without wgpu only the C struct is checked; with wgpu but no adapter the cases print SKIP and pass.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/capi/xtframe.h"

namespace {

/// The desc size an older host (before T-647) passes: everything up to `sample_count`.
constexpr uint32_t kOldDescSize = static_cast<uint32_t>(offsetof(xt_gpu_target_desc, sample_count));

}  // namespace

TEST_CASE("capi gpu text 0 (T-647): sample_count is appended after backend") {
    CHECK(offsetof(xt_gpu_target_desc, sample_count) >=
          offsetof(xt_gpu_target_desc, backend) + sizeof(void*));
    CHECK(sizeof(xt_gpu_target_desc) > kOldDescSize);
}

#ifdef XT_CAPI_TEST_HAS_WGPU

namespace {

constexpr int kW = 480;
constexpr int kH = 400;

using Image = std::vector<uint8_t>;

/// What a case region is held to.
enum class Rule {
    SameRaster,  ///< max channel difference <= 2
    Tolerance,   ///< 10 §10.10: <= 3 % of the pixels differ by more than 16
    Report,      ///< numbers printed only (24-96 px band, T-263c)
};

struct Region {
    const char* name;
    int x0, y0, x1, y1;
    Rule rule;
};

constexpr Region kStar = {"non-convex star", 0, 250, 140, 390, Rule::Tolerance};
constexpr Region kWord = {"120 px word", 150, 240, kW, kH, Rule::Tolerance};
constexpr Region kWhole = {"whole frame", 0, 0, kW, kH, Rule::Tolerance};

/// The fixture's case regions (text.layout.xts).
const Region kRegions[] = {
    {"light 12 px on dark", 0, 0, kW, 24, Rule::SameRaster},
    {"light 24 px on dark", 0, 24, kW, 56, Rule::Report},
    {"light 48 px on dark", 0, 56, kW, 120, Rule::Report},
    {"dark 12 px on transparent", 0, 120, kW, 144, Rule::SameRaster},
    {"dark 24 px on transparent", 0, 144, kW, 176, Rule::Report},
    {"dark 48 px on transparent", 0, 176, kW, 240, Rule::Report},
    kStar,
    kWord,
    kWhole,
};

xt_gpu_target_desc textDesc(int samples) {
    xt_gpu_target_desc desc;
    std::memset(&desc, 0, sizeof desc);
    desc.struct_size = sizeof(desc);
    desc.window.kind = XT_WINDOW_NONE;
    desc.width = kW;
    desc.height = kH;
    desc.backend = "";
    desc.sample_count = samples;
    return desc;
}

/// A target for `desc`, or null when this machine has no WebGPU adapter (the case is skipped).
xt_gpu_target* createOrSkip(const xt_gpu_target_desc& desc) {
    xt_gpu_target* t = xt_gpu_target_create(&desc);
    if (t == nullptr) {
        const std::string error = xt_capi_last_error();
        REQUIRE_MESSAGE(std::strstr(error.c_str(), "no WebGPU adapter") != nullptr, error);
        std::printf("SKIP: no WebGPU adapter (%s)\n", error.c_str());
    }
    return t;
}

/// The text bundle and its surfaces (destroyed before the bundle).
struct TextBundle {
    xt_bundle* bundle = nullptr;
    std::vector<xt_surface*> surfaces;
    TextBundle() {
        bundle = xt_bundle_open(XT_CAPI_TEST_TEXT_BUNDLE);
        REQUIRE_MESSAGE(bundle != nullptr, std::string(xt_capi_last_error()));
    }
    ~TextBundle() {
        for (xt_surface* s : surfaces) xt_surface_destroy(s);
        (void)xt_bundle_close(bundle);
    }
    TextBundle(const TextBundle&) = delete;
    TextBundle& operator=(const TextBundle&) = delete;
    xt_surface* surface(const char* ui = "text") {
        xt_surface_desc desc{};
        desc.struct_size = sizeof(desc);
        desc.width = kW;
        desc.height = kH;
        desc.device_pixel_ratio = 1.0f;
        desc.premultiplied = 1;
        desc.system_fonts = 0;  // the bundled Noto Sans only: both paths shape with the same font
        xt_surface* s = xt_surface_create(bundle, ui, &desc);
        REQUIRE_MESSAGE(s != nullptr, std::string(xt_capi_last_error()));
        surfaces.push_back(s);
        return s;
    }
    /// The SW reference: one update(0), premultiplied.
    Image software(const char* ui = "text") {
        xt_surface* s = surface(ui);
        Image px(static_cast<size_t>(kW) * kH * 4u, 0);
        REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
        REQUIRE(xt_surface_render(s, px.data(), kW * 4, nullptr) == XT_STATUS_OK);
        return px;
    }
    /// A new surface drawn 1:1 onto `t` (transparent clear), read back.
    Image gpu(xt_gpu_target* t, const char* ui = "text") {
        xt_surface* s = surface(ui);
        REQUIRE(xt_gpu_target_begin_frame(t, nullptr) == XT_STATUS_OK);
        REQUIRE(xt_surface_update(s, 0.0f) == XT_STATUS_OK);
        REQUIRE_MESSAGE(xt_surface_render_gpu(s, t, nullptr) == XT_STATUS_OK,
                        std::string(xt_capi_last_error()));
        REQUIRE(xt_gpu_target_end_frame(t) == XT_STATUS_OK);
        Image px(static_cast<size_t>(kW) * kH * 4u, 0);
        REQUIRE_MESSAGE(xt_gpu_target_readback(t, px.data(), kW * 4) == XT_STATUS_OK,
                        std::string(xt_capi_last_error()));
        return px;
    }
};

int channelDiff(const Image& a, const Image& b, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
    int d = 0;
    for (size_t c = 0; c < 4; ++c)
        d = std::max(d, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
    return d;
}

/// Prints the region's numbers and answers whether it meets its rule.
bool meetsRule(const Image& gpu, const Image& sw, const Region& r, int samples) {
    int over = 0, differ = 0, maxDiff = 0;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const int d = channelDiff(gpu, sw, x, y);
            differ += d != 0 ? 1 : 0;
            over += d > 16 ? 1 : 0;
            maxDiff = std::max(maxDiff, d);
        }
    }
    const int area = (r.x1 - r.x0) * (r.y1 - r.y0);
    const double percent = 100.0 * over / area;
    std::printf("sample_count %d, %-26s: %5d px differ, max %3d, > 16: %4d (%.2f %%)\n", samples, r.name,
                differ, maxDiff, over, percent);
    switch (r.rule) {
        case Rule::SameRaster:
            return maxDiff <= 2;
        case Rule::Tolerance:
            return percent <= 3.0;
        case Rule::Report:
            break;
    }
    return true;
}

/// Pixels of the region with an alpha strictly between 0 and 255 (antialiased edges over transparent).
int intermediateAlpha(const Image& px, const Region& r) {
    int n = 0;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const uint8_t a = px[(static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u + 3u];
            n += a > 0 && a < 255 ? 1 : 0;
        }
    }
    return n;
}

}  // namespace

TEST_CASE(
    "capi gpu text 1 (T-647): the text fixture on the GPU target is within 10 §10.10 of SW, 1x and 4x") {
    TextBundle b;
    const Image sw = b.software();
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        xt_gpu_target* t = createOrSkip(textDesc(samples));
        if (t == nullptr) return;
        const Image gpu = b.gpu(t);
        for (const Region& r : kRegions) {
            const std::string name = r.name;
            CAPTURE(name);
            CHECK(meetsRule(gpu, sw, r, samples));
        }
        for (xt_surface* s : b.surfaces) (void)xt_surface_detach_gpu(s);
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
}

TEST_CASE(
    "capi gpu text 2 (T-647): at sample_count 4 the non-convex path and the 120 px word are antialiased") {
    TextBundle b;
    const Image sw = b.software();
    xt_gpu_target* one = createOrSkip(textDesc(1));
    if (one == nullptr) return;
    xt_gpu_target* four = createOrSkip(textDesc(4));
    REQUIRE(four != nullptr);
    const Image hard = b.gpu(one);
    const Image soft = b.gpu(four);
    for (const Region& r : {kStar, kWord}) {
        const std::string name = r.name;
        CAPTURE(name);
        const int ref = intermediateAlpha(sw, r), at1 = intermediateAlpha(hard, r),
                  at4 = intermediateAlpha(soft, r);
        std::printf("%-15s intermediate-alpha px: sw %d, sample_count 1: %d, sample_count 4: %d\n", r.name,
                    ref, at1, at4);
        CHECK(ref > 0);
        CHECK(at4 * 2 >= ref);  // 4x resolves edges to the SW coverage's order
        CHECK(at4 > at1);       // single-sampled stencil-then-cover is a hard edge
    }
    for (xt_surface* s : b.surfaces) (void)xt_surface_detach_gpu(s);
    CHECK(xt_gpu_target_destroy(one) == XT_STATUS_OK);
    CHECK(xt_gpu_target_destroy(four) == XT_STATUS_OK);
}

TEST_CASE(
    "capi gpu text 3 (T-647): sample_count 3 is refused, 0 means 4, an older struct_size still creates") {
    for (const int bad : {3, 2, 8, -1}) {
        CAPTURE(bad);
        const xt_gpu_target_desc desc = textDesc(bad);
        CHECK(xt_gpu_target_create(&desc) == nullptr);
        CHECK(std::strstr(xt_capi_last_error(), "sample_count") != nullptr);
    }
    TextBundle b;
    xt_gpu_target* zero = createOrSkip(textDesc(0));
    if (zero == nullptr) return;
    xt_gpu_target* four = createOrSkip(textDesc(4));
    REQUIRE(four != nullptr);
    CHECK(b.gpu(zero) == b.gpu(four));
    // A desc ending before sample_count (an older host): the field is not read, the target is 4x.
    xt_gpu_target_desc old = textDesc(3);  // garbage past the old size must be ignored
    old.struct_size = kOldDescSize;
    xt_gpu_target* legacy = xt_gpu_target_create(&old);
    REQUIRE_MESSAGE(legacy != nullptr, std::string(xt_capi_last_error()));
    CHECK(b.gpu(legacy) == b.gpu(four));
    xt_gpu_target_desc tiny = textDesc(0);
    tiny.struct_size = kOldDescSize - 1;
    CHECK(xt_gpu_target_create(&tiny) == nullptr);
    CHECK(std::strstr(xt_capi_last_error(), "struct_size") != nullptr);
    for (xt_surface* s : b.surfaces) (void)xt_surface_detach_gpu(s);
    CHECK(xt_gpu_target_destroy(zero) == XT_STATUS_OK);
    CHECK(xt_gpu_target_destroy(four) == XT_STATUS_OK);
    CHECK(xt_gpu_target_destroy(legacy) == XT_STATUS_OK);
}

namespace {

/// T-653: the `skew` UI of the same bundle -- three 120 px bands (axis-aligned, skewX(-6deg),
/// rotate(3deg)), each a note with a 2 px left border, 12 / 24 px text and a 1 px box.
const Region kSkewRegions[] = {
    {"upright band", 0, 0, kW, 130, Rule::Tolerance},
    {"skewX(-6deg) band", 0, 130, kW, 260, Rule::Tolerance},
    {"rotate(3deg) band", 0, 260, kW, kH, Rule::Tolerance},
    kWhole,
};

/// Whether pixel `i` of `px` is within 24 of the opaque colour `rgb` on every channel.
bool nearColor(const Image& px, size_t i, uint32_t rgb) {
    const int dr = std::abs(px[i] - static_cast<int>(rgb >> 16)),
              dg = std::abs(px[i + 1] - static_cast<int>((rgb >> 8) & 0xffu)),
              db = std::abs(px[i + 2] - static_cast<int>(rgb & 0xffu));
    return std::max(dr, std::max(dg, db)) <= 24;
}

/// Pixels of the band whose colour is the border colour #b04020.
int borderPixels(const Image& px, const Region& r) {
    int n = 0;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            n +=
                nearColor(px, (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u, 0xb04020u) ? 1 : 0;
        }
    }
    return n;
}

/// Antialiased edge pixels of the band: near none of the fixture's flat colours (the background,
/// the text, the left border, the box border).
int blendedPixels(const Image& px, const Region& r) {
    int n = 0;
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            const size_t i = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 4u;
            const bool flat = nearColor(px, i, 0xe8dcc4u) || nearColor(px, i, 0x203030u) ||
                              nearColor(px, i, 0xb04020u) || nearColor(px, i, 0x405060u);
            n += flat ? 0 : 1;
        }
    }
    return n;
}

}  // namespace

TEST_CASE(
    "capi gpu text 4 (T-653): skewed / rotated text and thin borders on the GPU target are within 10 "
    "§10.10 of SW, 1x and 4x") {
    TextBundle b;
    const Image sw = b.software("skew");
    for (const int samples : {1, 4}) {
        CAPTURE(samples);
        xt_gpu_target* t = createOrSkip(textDesc(samples));
        if (t == nullptr) return;
        const Image gpu = b.gpu(t, "skew");
        for (const Region& r : kSkewRegions) {
            const std::string name = r.name;
            CAPTURE(name);
            CHECK(meetsRule(gpu, sw, r, samples));
        }
        // The 2 px left border of every band is drawn (a one-sided border ring drew nothing on a
        // single-sampled target before T-653, transformed or not).
        for (const Region& r : {kSkewRegions[0], kSkewRegions[1], kSkewRegions[2]}) {
            const std::string name = r.name;
            CAPTURE(name);
            const int ref = borderPixels(sw, r), got = borderPixels(gpu, r);
            const int refAa = blendedPixels(sw, r), gotAa = blendedPixels(gpu, r);
            std::printf("sample_count %d, %-26s: border px sw %d, gpu %d; blended px sw %d, gpu %d\n",
                        samples, r.name, ref, got, refAa, gotAa);
            CHECK(ref > 0);
            CHECK(got * 2 >= ref);
            // The transformed glyphs are antialiased at 1x too (stair-stepped before T-653).
            CHECK(gotAa * 4 >= refAa * 3);
        }
        for (xt_surface* s : b.surfaces) (void)xt_surface_detach_gpu(s);
        CHECK(xt_gpu_target_destroy(t) == XT_STATUS_OK);
    }
}

#endif  // XT_CAPI_TEST_HAS_WGPU
