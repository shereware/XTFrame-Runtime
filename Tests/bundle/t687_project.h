#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "xt/sw/sw_png.h"

/// T-687: a small UI Project for the BC7 pack tests (Tests/bundle/test_asset_pipeline.cpp and the
/// end-to-end draw in Tests/wgpu/test_wgpu_pack_bc7.cpp): one UI `m` (640 x 320) drawing
///   - `img/bg.png`   512 x 288, opaque, photo-like (smooth gradients and mild noise) -- a background;
///   - `img/art.png`  302 x 300, a soft-edged disc with alpha, its transparent texels carrying green
///                    (what straight filtering would drag into the edge without the bleed); 302 is not
///                    a multiple of 4, so it is padded;
///   - `img/wide.png` 300 x 255 and `img/icon.png` 64 x 64: under `auto` they stay PNG.
/// `publishJson` is the `.xtui`'s `publish` object ("" = none).

namespace xt::test::t687 {

struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> rgba;
};

inline uint32_t lcg(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}

inline uint8_t clampByte(double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0) + 0.5); }

inline Image background(int w = 512, int h = 288, uint32_t seed = 7) {
    Image img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * static_cast<size_t>(h) * 4)};
    uint32_t state = seed;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.rgba.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            const double n = static_cast<double>(lcg(state) % 9) - 4.0;
            p[0] = clampByte(110 + 70 * std::sin(x / 41.0) + 30 * std::cos(y / 29.0) + n);
            p[1] = clampByte(90 + 50 * std::sin((x + y) / 53.0) + n);
            p[2] = clampByte(140 + 60 * std::cos(x / 67.0 - y / 31.0) + n);
            p[3] = 255;
        }
    }
    return img;
}

inline Image art(int w = 302, int h = 300) {
    Image img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * static_cast<size_t>(h) * 4)};
    const double cx = w / 2.0, cy = h / 2.0, r = std::min(w, h) * 0.42;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img.rgba.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const uint8_t a = clampByte((r - d) * 24.0);
            if (a == 0) {
                p[0] = 0, p[1] = 255, p[2] = 0, p[3] = 0;  // the garbage straight filtering would show
            } else {
                p[0] = clampByte(200 - 0.3 * x + 20 * std::sin(y / 17.0));
                p[1] = clampByte(60 + 0.2 * y);
                p[2] = clampByte(80 + 40 * std::cos(x / 23.0));
                p[3] = a;
            }
        }
    }
    return img;
}

inline Image flat(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Image img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * static_cast<size_t>(h) * 4)};
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = r, img.rgba[i + 1] = g, img.rgba[i + 2] = b, img.rgba[i + 3] = a;
    }
    return img;
}

inline bool writeImage(const std::filesystem::path& path, const Image& img) {
    return sw::writePng(path.string(), img.rgba.data(), img.w, img.h, img.w * 4);
}

/// Writes the project into `dir` (which must exist and be empty); false when a file could not be written.
inline bool writeProject(const std::filesystem::path& dir, const std::string& publishJson) {
    std::error_code ec;
    std::filesystem::create_directories(dir / "m" / "img", ec);
    {
        std::ofstream xtui(dir / "t687.xtui", std::ios::binary | std::ios::trunc);
        xtui
            << R"({ "version": 1, "name": "T687", "id": "t687", "uis": [ { "id": "m", "doc": "m/m.xt", "size": [640, 320] } ])";
        if (!publishJson.empty()) {
            xtui << R"(, "publish": )" << publishJson;
        }
        xtui << " }\n";
    }
    {
        std::ofstream xt(dir / "m" / "m.xt", std::ios::binary | std::ios::trunc);
        xt << "<xt>\n<head><style>\n"
              "#bg { position: absolute; left: 0px; top: 0px; width: 512px; height: 288px; }\n"
              "#art { position: absolute; left: 330px; top: 10px; width: 302px; height: 300px; }\n"
              "#wide { position: absolute; left: 520px; top: 200px; width: 100px; height: 85px; }\n"
              "#icon { position: absolute; left: 10px; top: 250px; width: 64px; height: 64px; }\n"
              "#small { position: absolute; left: 100px; top: 200px; width: 121px; height: 120px; }\n"
              "</style></head>\n<body>\n"
              "<img id=\"bg\" src=\"img/bg.png\"/>\n<img id=\"art\" src=\"img/art.png\"/>\n"
              "<img id=\"wide\" src=\"img/wide.png\"/>\n<img id=\"icon\" src=\"img/icon.png\"/>\n"
              "<img id=\"small\" src=\"img/art.png\"/>\n"
              "</body>\n</xt>\n";
    }
    return writeImage(dir / "m" / "img" / "bg.png", background()) &&
           writeImage(dir / "m" / "img" / "art.png", art()) &&
           writeImage(dir / "m" / "img" / "wide.png", background(300, 255, 11)) &&
           writeImage(dir / "m" / "img" / "icon.png", art(64, 64));
}

}  // namespace xt::test::t687
