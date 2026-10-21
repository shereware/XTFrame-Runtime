#include "texture_compress.h"

#include <algorithm>
#include <cstring>
#include <string>

#include "bc7enc.h"
#include "rgbcx.h"
#include "xt/dds.h"

/// See texture_compress.h. BC1/BC3 use rgbcx at its highest quality level (small UI images, so
/// encode time is not a concern); BC7 uses bc7enc's default (perceptual) parameters.

namespace xt::bundle {

namespace {

void writeLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

constexpr uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

void ensureEncoders() {
    static bool initialized = false;
    if (!initialized) {
        rgbcx::init();
        bc7enc_compress_block_init();
        initialized = true;
    }
}

/// Encodes one `w` x `h` level; blocks in row order, edge blocks padded by clamping.
void encodeLevel(const uint8_t* rgba, int w, int h, BcFormat format, std::vector<uint8_t>& out) {
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    const int blocksX = (w + 3) / 4;
    const int blocksY = (h + 3) / 4;
    uint8_t pixels[16 * 4];
    uint8_t block[16];
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            for (int ty = 0; ty < 4; ++ty) {
                const int y = std::min(by * 4 + ty, h - 1);
                for (int tx = 0; tx < 4; ++tx) {
                    const int x = std::min(bx * 4 + tx, w - 1);
                    std::memcpy(
                        pixels + (ty * 4 + tx) * 4,
                        rgba + (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4,
                        4);
                }
            }
            switch (format) {
                case BcFormat::Bc1:
                    rgbcx::encode_bc1(rgbcx::MAX_LEVEL, block, pixels, /*allow_3color=*/true,
                                      /*use_transparent_texels_for_black=*/false);
                    out.insert(out.end(), block, block + 8);
                    break;
                case BcFormat::Bc3:
                    rgbcx::encode_bc3(rgbcx::MAX_LEVEL, block, pixels);
                    out.insert(out.end(), block, block + 16);
                    break;
                case BcFormat::Bc7:
                default:
                    bc7enc_compress_block(block, pixels, &params);
                    out.insert(out.end(), block, block + 16);
                    break;
            }
        }
    }
}

/// 2x box downsample (straight alpha; colors weighted by alpha so transparent texels do not bleed).
std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, int w, int h, int& outW, int& outH) {
    outW = std::max(1, w / 2);
    outH = std::max(1, h / 2);
    std::vector<uint8_t> dst(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
    for (int y = 0; y < outH; ++y) {
        for (int x = 0; x < outW; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0};
            uint32_t alphaSum = 0;
            int n = 0;
            for (int dy = 0; dy < 2; ++dy) {
                const int sy = std::min(y * 2 + dy, h - 1);
                for (int dx = 0; dx < 2; ++dx) {
                    const int sx = std::min(x * 2 + dx, w - 1);
                    const uint8_t* p =
                        src.data() +
                        (static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)) * 4;
                    const uint32_t a = p[3];
                    sum[0] += p[0] * a;
                    sum[1] += p[1] * a;
                    sum[2] += p[2] * a;
                    alphaSum += a;
                    ++n;
                }
            }
            uint8_t* d = dst.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(outW) + static_cast<size_t>(x)) * 4;
            if (alphaSum == 0) {
                d[0] = d[1] = d[2] = d[3] = 0;
            } else {
                d[0] = static_cast<uint8_t>(sum[0] / alphaSum);
                d[1] = static_cast<uint8_t>(sum[1] / alphaSum);
                d[2] = static_cast<uint8_t>(sum[2] / alphaSum);
                d[3] = static_cast<uint8_t>(alphaSum / static_cast<uint32_t>(n));
            }
        }
    }
    return dst;
}

}  // namespace

bool parseBcFormat(std::string_view name, BcFormat& out) {
    std::string lower(name);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (lower == "none") {
        out = BcFormat::None;
    } else if (lower == "bc1") {
        out = BcFormat::Bc1;
    } else if (lower == "bc3") {
        out = BcFormat::Bc3;
    } else if (lower == "bc7") {
        out = BcFormat::Bc7;
    } else {
        return false;
    }
    return true;
}

const char* bcFormatName(BcFormat format) {
    switch (format) {
        case BcFormat::Bc1:
            return "bc1";
        case BcFormat::Bc3:
            return "bc3";
        case BcFormat::Bc7:
            return "bc7";
        default:
            return "none";
    }
}

bool imageHasAlpha(const uint8_t* rgba, int w, int h) {
    const size_t count = static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h));
    for (size_t i = 0; i < count; ++i) {
        if (rgba[i * 4 + 3] != 255) {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> encodeDds(const uint8_t* rgba, int w, int h, BcFormat format, bool mipmaps) {
    std::vector<uint8_t> out;
    if (rgba == nullptr || w <= 0 || h <= 0 || format == BcFormat::None) {
        return out;
    }
    ensureEncoders();
    const DdsFormat dds = format == BcFormat::Bc1
                              ? DdsFormat::Bc1
                              : (format == BcFormat::Bc3 ? DdsFormat::Bc3 : DdsFormat::Bc7);
    int mipCount = 1;
    if (mipmaps) {
        int mw = w;
        int mh = h;
        while (mw > 1 || mh > 1) {
            mw = std::max(1, mw / 2);
            mh = std::max(1, mh / 2);
            ++mipCount;
        }
    }
    constexpr uint32_t kCaps = 0x1, kHeight = 0x2, kWidth = 0x4, kPixelFormat = 0x1000, kLinearSize = 0x80000,
                       kMipmapCount = 0x20000;
    constexpr uint32_t kCapsTexture = 0x1000, kCapsMipmap = 0x400000, kCapsComplex = 0x8;
    out.reserve(148 + ddsLevelSize(dds, w, h) * 4 / 3);
    writeLe32(out, fourCc('D', 'D', 'S', ' '));
    writeLe32(out, 124);
    writeLe32(out, kCaps | kHeight | kWidth | kPixelFormat | kLinearSize | (mipCount > 1 ? kMipmapCount : 0));
    writeLe32(out, static_cast<uint32_t>(h));
    writeLe32(out, static_cast<uint32_t>(w));
    writeLe32(out, static_cast<uint32_t>(ddsLevelSize(dds, w, h)));
    writeLe32(out, 0);  // depth
    writeLe32(out, static_cast<uint32_t>(mipCount));
    for (int i = 0; i < 11; ++i) {
        writeLe32(out, 0);  // reserved1
    }
    // DDS_PIXELFORMAT.
    writeLe32(out, 32);
    writeLe32(out, 0x4);  // DDPF_FOURCC
    writeLe32(out, format == BcFormat::Bc1   ? fourCc('D', 'X', 'T', '1')
                   : format == BcFormat::Bc3 ? fourCc('D', 'X', 'T', '5')
                                             : fourCc('D', 'X', '1', '0'));
    for (int i = 0; i < 5; ++i) {
        writeLe32(out, 0);  // bit count + masks
    }
    writeLe32(out, kCapsTexture | (mipCount > 1 ? (kCapsMipmap | kCapsComplex) : 0));
    writeLe32(out, 0);  // caps2
    writeLe32(out, 0);  // caps3
    writeLe32(out, 0);  // caps4
    writeLe32(out, 0);  // reserved2
    if (format == BcFormat::Bc7) {
        writeLe32(out, 98);  // DXGI_FORMAT_BC7_UNORM
        writeLe32(out, 3);   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        writeLe32(out, 0);   // miscFlag
        writeLe32(out, 1);   // arraySize
        writeLe32(out, 0);   // miscFlags2
    }
    std::vector<uint8_t> level(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    int lw = w;
    int lh = h;
    for (int i = 0; i < mipCount; ++i) {
        encodeLevel(level.data(), lw, lh, format, out);
        if (i + 1 < mipCount) {
            int nw = 0;
            int nh = 0;
            level = downsample(level, lw, lh, nw, nh);
            lw = nw;
            lh = nh;
        }
    }
    return out;
}

}  // namespace xt::bundle
