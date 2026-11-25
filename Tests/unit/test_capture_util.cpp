#include <cstdint>
#include <string>
#include <vector>

#include "doctest.h"
#include "xt/sw/capture_util.h"
#include "xt/sw/sw_png.h"

/// T-879 (Docs/design/59-visual-feedback.md §59.3): view.capture's pixel tools -- downscaleToMaxEdge,
/// crop, composeSheet, drawLabel -- and the JPEG encoder.

using namespace xt;
using namespace xt::sw;

namespace {

CaptureImage solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    CaptureImage img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = r;
        img.rgba[i + 1] = g;
        img.rgba[i + 2] = b;
        img.rgba[i + 3] = a;
    }
    return img;
}

const uint8_t* px(const CaptureImage& img, int x, int y) {
    return &img.rgba[(static_cast<size_t>(y) * static_cast<size_t>(img.width) + static_cast<size_t>(x)) * 4];
}

uint64_t fnv1a(const std::vector<uint8_t>& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) {
        h = (h ^ b) * 1099511628211ull;
    }
    return h;
}

}  // namespace

TEST_CASE("T-879 downscaleToMaxEdge: the long edge, never enlarging, area averages in premultiplied space") {
    CaptureImage wide = solid(400, 100, 10, 20, 30, 255);
    const CaptureImage same = downscaleToMaxEdge(wide, 400);
    CHECK(same.width == 400);
    CHECK(downscaleToMaxEdge(wide, 0).width == 400);
    CHECK(downscaleToMaxEdge(wide, 1000).width == 400);  // Never enlarged.
    const CaptureImage half = downscaleToMaxEdge(wide, 200);
    CHECK(half.width == 200);
    CHECK(half.height == 50);
    const CaptureImage tall = downscaleToMaxEdge(solid(30, 90, 0, 0, 0, 255), 45);
    CHECK(tall.width == 15);
    CHECK(tall.height == 45);

    // A 2 x 2 block of white and transparent pixels averages to white at half alpha: transparent
    // pixels do not darken it (premultiplied).
    CaptureImage block = solid(4, 4, 0, 0, 0, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            if ((x + y) % 2 == 0) {
                uint8_t* p = &block.rgba[(static_cast<size_t>(y) * 4 + static_cast<size_t>(x)) * 4];
                p[0] = p[1] = p[2] = p[3] = 255;
            }
        }
    }
    const CaptureImage two = downscaleToMaxEdge(block, 2);
    REQUIRE(two.width == 2);
    CHECK(px(two, 0, 0)[0] == 255);
    CHECK(px(two, 0, 0)[3] == 128);
    // A red/blue split over a 3:1 area averages 3/4 red.
    CaptureImage split = solid(4, 1, 0, 0, 255, 255);
    for (int x = 0; x < 3; ++x) {
        uint8_t* p = &split.rgba[static_cast<size_t>(x) * 4];
        p[0] = 255;
        p[2] = 0;
    }
    const CaptureImage one = downscaleToMaxEdge(split, 1);
    REQUIRE(one.width == 1);
    CHECK(px(one, 0, 0)[0] == 191);
    CHECK(px(one, 0, 0)[2] == 64);
}

TEST_CASE("T-879 crop: the part inside, rounded outwards; nothing inside is an error") {
    CaptureImage img = solid(10, 10, 0, 0, 0, 255);
    img.rgba[(static_cast<size_t>(2) * 10 + 3) * 4] = 200;  // (3, 2)
    CaptureImage out;
    REQUIRE(crop(img, Rect{3, 2, 4, 3}, out) == Status::kOk);
    CHECK(out.width == 4);
    CHECK(out.height == 3);
    CHECK(px(out, 0, 0)[0] == 200);
    REQUIRE(crop(img, Rect{2.5f, 1.5f, 1.0f, 1.0f}, out) == Status::kOk);
    CHECK(out.width == 2);  // 2..4 rounded outwards
    CHECK(out.height == 2);
    REQUIRE(crop(img, Rect{8, 8, 10, 10}, out) == Status::kOk);
    CHECK(out.width == 2);  // Clipped to the image.
    CHECK(crop(img, Rect{20, 20, 5, 5}, out) == Status::kInvalidArgument);
    CHECK(crop(img, Rect{1, 1, 0, 5}, out) == Status::kInvalidArgument);
    CHECK(out.width == 2);  // Left unchanged.
}

TEST_CASE("T-879 composeSheet: size, cell origins, the grey gap and the labels") {
    std::vector<CaptureImage> frames;
    for (int i = 0; i < 5; ++i) {
        frames.push_back(solid(20, 10, static_cast<uint8_t>(i * 40), 0, 0, 255));
    }
    const std::vector<std::string> labels{"0.00s", "", "", "", ""};
    CaptureImage sheet;
    REQUIRE(composeSheet(frames, labels, 0, sheet) == Status::kOk);
    // ceil(sqrt(5)) = 3 columns, 2 rows.
    CHECK(sheet.width == 3 * 20 + 2 * kSheetGap);
    CHECK(sheet.height == 2 * 10 + 1 * kSheetGap);
    CHECK(px(sheet, 20 + kSheetGap, 0)[0] == 40);   // Cell 1's origin.
    CHECK(px(sheet, 0, 10 + kSheetGap)[0] == 120);  // Cell 3's origin (second row).
    CHECK(px(sheet, 5, 10 + 1)[0] == 0x80);         // The gap between the rows.
    CHECK(px(sheet, 5, 10 + 1)[3] == 0xFF);
    CHECK(px(sheet, sheet.width - 1, sheet.height - 1)[0] == 0x80);  // The empty sixth cell.
    CHECK(px(sheet, 3, 2)[0] == 0xFF);                               // The label's first dot of "0" (white).
    CHECK(px(sheet, 2, 1)[0] == 0x00);                               // Its black border.

    // A transparent frame shows the grey.
    std::vector<CaptureImage> clear{solid(4, 4, 255, 0, 0, 0)};
    REQUIRE(composeSheet(clear, {}, 0, sheet) == Status::kOk);
    CHECK(px(sheet, 1, 1)[0] == 0x80);
    CHECK(px(sheet, 1, 1)[3] == 0xFF);

    std::vector<CaptureImage> mixed{solid(4, 4, 0, 0, 0, 255), solid(5, 4, 0, 0, 0, 255)};
    CHECK(composeSheet(mixed, {}, 0, sheet) == Status::kInvalidArgument);
    CHECK(composeSheet({}, {}, 0, sheet) == Status::kInvalidArgument);
}

TEST_CASE("T-879 drawLabel: the built-in dot font, byte for byte") {
    CHECK(labelWidth("0.25s", 1) == 29);
    CHECK(labelWidth("0.25s", 2) == 58);
    CaptureImage img = solid(64, 12, 0x80, 0x80, 0x80, 255);
    drawLabel(img, 1, 1, "0.25s");
    CHECK(fnv1a(img.rgba) == 0x8ea7191413019743ull);
    CaptureImage big = solid(80, 20, 0x80, 0x80, 0x80, 255);
    drawLabel(big, 1, 1, "t-1:az", 2);
    CHECK(fnv1a(big.rgba) == 0x81c4529c6152d033ull);
    // Clipped at the edges, no write outside.
    CaptureImage tiny = solid(3, 3, 0x80, 0x80, 0x80, 255);
    drawLabel(tiny, -2, -2, "88");
    CHECK(tiny.rgba.size() == 36);
}

TEST_CASE("T-879 encodeJpeg: baseline JPEG bytes") {
    std::vector<uint8_t> rgb(8 * 8 * 3, 128);
    std::vector<uint8_t> jpeg;
    REQUIRE(encodeJpeg(rgb.data(), 8, 8, 80, jpeg));
    REQUIRE(jpeg.size() > 4);
    CHECK(jpeg[0] == 0xFF);
    CHECK(jpeg[1] == 0xD8);
    CHECK_FALSE(encodeJpeg(nullptr, 8, 8, 80, jpeg));
}
