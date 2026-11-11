#include "texture_compress.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "bc7enc.h"
#include "bundle_json.h"
#include "rgbcx.h"
#include "xt/compile/utf8_path.h"
#include "xt/dds.h"
#include "xt/util/hash.h"

/// See texture_compress.h. BC1/BC3 use rgbcx at its highest quality level (small UI images, so
/// encode time is not a concern); BC7 uses bc7enc's default (perceptual) parameters.

namespace xt::bundle {

const char* const kPublishEncoderVersion = "bc7enc_rdo-b943862+rgbcx/default-params;xt-publish-1";

namespace {

void texWriteLe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void texWriteLe64(std::vector<uint8_t>& out, uint64_t v) {
    texWriteLe32(out, static_cast<uint32_t>(v & 0xFFFFFFFFu));
    texWriteLe32(out, static_cast<uint32_t>(v >> 32));
}

uint32_t texReadLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t texReadLe64(const uint8_t* p) {
    return static_cast<uint64_t>(texReadLe32(p)) | (static_cast<uint64_t>(texReadLe32(p + 4)) << 32);
}

constexpr uint32_t texFourCc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

/// T-652: once per process under std::call_once (two threads may pack at the same time); the encoder
/// tables are read-only afterwards.
void ensureEncoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init();
        bc7enc_compress_block_init();
    });
}

/// `num / den` rounded to nearest, ties to even (`den` > 0).
uint32_t divRoundHalfEven(uint32_t num, uint32_t den) {
    const uint32_t q = num / den;
    const uint32_t r = num % den;
    const uint32_t twice = r * 2u;
    return q + ((twice > den || (twice == den && (q & 1u) != 0)) ? 1u : 0u);
}

size_t blockBytesOf(BcFormat format) { return format == BcFormat::Bc1 ? 8u : 16u; }

/// One level to encode: `rgba` (`w` x `h`) whose blocks go at `offset` of the output.
struct LevelRef {
    const uint8_t* rgba = nullptr;
    int w = 0;
    int h = 0;
    size_t offset = 0;
};

/// Encodes block row `by` of `level` into `dst` (blocks in row order, edge blocks padded by clamping).
void encodeBlockRow(const LevelRef& level, int by, BcFormat format, uint8_t* dst) {
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);
    const int blocksX = (level.w + 3) / 4;
    const size_t step = blockBytesOf(format);
    uint8_t pixels[16 * 4];
    for (int bx = 0; bx < blocksX; ++bx) {
        for (int ty = 0; ty < 4; ++ty) {
            const int y = std::min(by * 4 + ty, level.h - 1);
            for (int tx = 0; tx < 4; ++tx) {
                const int x = std::min(bx * 4 + tx, level.w - 1);
                std::memcpy(
                    pixels + (ty * 4 + tx) * 4,
                    level.rgba +
                        (static_cast<size_t>(y) * static_cast<size_t>(level.w) + static_cast<size_t>(x)) * 4,
                    4);
            }
        }
        uint8_t* block = dst + static_cast<size_t>(bx) * step;
        switch (format) {
            case BcFormat::Bc1:
                rgbcx::encode_bc1(rgbcx::MAX_LEVEL, block, pixels, /*allow_3color=*/true,
                                  /*use_transparent_texels_for_black=*/false);
                break;
            case BcFormat::Bc3:
                rgbcx::encode_bc3(rgbcx::MAX_LEVEL, block, pixels);
                break;
            case BcFormat::Bc7:
            default:
                bc7enc_compress_block(block, pixels, &params);
                break;
        }
    }
}

/// Encodes every block row of every level into `out` (already sized), on up to `threads` threads.
/// Each block depends only on its 16 texels, so the bytes do not depend on the thread count.
void encodeLevels(const std::vector<LevelRef>& levels, BcFormat format, int threads, uint8_t* out) {
    struct Row {
        size_t level;
        int by;
    };
    std::vector<Row> rows;
    for (size_t i = 0; i < levels.size(); ++i) {
        const int blocksY = (levels[i].h + 3) / 4;
        for (int by = 0; by < blocksY; ++by) {
            rows.push_back(Row{i, by});
        }
    }
    auto rowDst = [&](const Row& row) {
        const LevelRef& level = levels[row.level];
        const size_t rowBytes = static_cast<size_t>((level.w + 3) / 4) * blockBytesOf(format);
        return out + level.offset + static_cast<size_t>(row.by) * rowBytes;
    };
    std::atomic<size_t> next{0};
    auto work = [&] {
        for (size_t i = next.fetch_add(1); i < rows.size(); i = next.fetch_add(1)) {
            encodeBlockRow(levels[rows[i].level], rows[i].by, format, rowDst(rows[i]));
        }
    };
    const size_t wanted = std::min(rows.size(), static_cast<size_t>(std::max(1, threads)));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < wanted; ++t) {
        try {
            pool.emplace_back(work);
        } catch (const std::system_error&) {
            break;  // Fewer threads: the remaining rows go to the ones that started.
        }
    }
    work();
    for (std::thread& thread : pool) {
        thread.join();
    }
}

DdsFormat ddsFormatOf(BcFormat format) {
    return format == BcFormat::Bc1 ? DdsFormat::Bc1
                                   : (format == BcFormat::Bc3 ? DdsFormat::Bc3 : DdsFormat::Bc7);
}

/// The 148-byte (BC7, DX10) or 128-byte (BC1 / BC3, FourCC) header; `logicalW` > 0 writes the T-686
/// logical-size marker into `dwReserved1[0..2]`.
void writeDdsHeader(std::vector<uint8_t>& out, BcFormat format, int w, int h, int mipCount, int logicalW,
                    int logicalH) {
    constexpr uint32_t kCaps = 0x1, kHeight = 0x2, kWidth = 0x4, kPixelFormat = 0x1000, kLinearSize = 0x80000,
                       kMipmapCount = 0x20000;
    constexpr uint32_t kCapsTexture = 0x1000, kCapsMipmap = 0x400000, kCapsComplex = 0x8;
    const DdsFormat dds = ddsFormatOf(format);
    texWriteLe32(out, texFourCc('D', 'D', 'S', ' '));
    texWriteLe32(out, 124);
    texWriteLe32(out,
                 kCaps | kHeight | kWidth | kPixelFormat | kLinearSize | (mipCount > 1 ? kMipmapCount : 0));
    texWriteLe32(out, static_cast<uint32_t>(h));
    texWriteLe32(out, static_cast<uint32_t>(w));
    texWriteLe32(out, static_cast<uint32_t>(ddsLevelSize(dds, w, h)));
    texWriteLe32(out, 0);  // depth
    texWriteLe32(out, static_cast<uint32_t>(mipCount));
    if (logicalW > 0 && logicalH > 0) {
        texWriteLe32(out, kDdsLogicalSizeTag);  // T-686 / T-687 (10 §10.13 ruling 2)
        texWriteLe32(out, static_cast<uint32_t>(logicalW));
        texWriteLe32(out, static_cast<uint32_t>(logicalH));
    } else {
        for (int i = 0; i < 3; ++i) {
            texWriteLe32(out, 0);
        }
    }
    for (int i = 3; i < 11; ++i) {
        texWriteLe32(out, 0);  // reserved1
    }
    // DDS_PIXELFORMAT.
    texWriteLe32(out, 32);
    texWriteLe32(out, 0x4);  // DDPF_FOURCC
    texWriteLe32(out, format == BcFormat::Bc1   ? texFourCc('D', 'X', 'T', '1')
                      : format == BcFormat::Bc3 ? texFourCc('D', 'X', 'T', '5')
                                                : texFourCc('D', 'X', '1', '0'));
    for (int i = 0; i < 5; ++i) {
        texWriteLe32(out, 0);  // bit count + masks
    }
    texWriteLe32(out, kCapsTexture | (mipCount > 1 ? (kCapsMipmap | kCapsComplex) : 0));
    texWriteLe32(out, 0);  // caps2
    texWriteLe32(out, 0);  // caps3
    texWriteLe32(out, 0);  // caps4
    texWriteLe32(out, 0);  // reserved2
    if (format == BcFormat::Bc7) {
        texWriteLe32(out, 98);  // DXGI_FORMAT_BC7_UNORM
        texWriteLe32(out, 3);   // D3D10_RESOURCE_DIMENSION_TEXTURE2D
        texWriteLe32(out, 0);   // miscFlag
        texWriteLe32(out, 1);   // arraySize
        texWriteLe32(out, 0);   // miscFlags2
    }
}

/// Header + the given levels (level 0 first, hardware sizes) encoded on `threads` threads.
std::vector<uint8_t> writeDdsFile(const std::vector<std::vector<uint8_t>>& pixels,
                                  const std::vector<std::pair<int, int>>& sizes, BcFormat format, int threads,
                                  int logicalW, int logicalH) {
    std::vector<uint8_t> out;
    const DdsFormat dds = ddsFormatOf(format);
    writeDdsHeader(out, format, sizes[0].first, sizes[0].second, static_cast<int>(sizes.size()), logicalW,
                   logicalH);
    std::vector<LevelRef> levels;
    size_t offset = out.size();
    for (size_t i = 0; i < sizes.size(); ++i) {
        levels.push_back(LevelRef{pixels[i].data(), sizes[i].first, sizes[i].second, offset});
        offset += ddsLevelSize(dds, sizes[i].first, sizes[i].second);
    }
    out.resize(offset, 0);
    encodeLevels(levels, format, threads, out.data());
    return out;
}

constexpr uint32_t kCacheFormatVersion = 1;
constexpr size_t kCacheHeadBytes = 4 + 4 + 4;  // magic, version, key length

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

std::vector<uint8_t> downsampleMipLevel(const uint8_t* rgba, int w, int h, int& outW, int& outH) {
    outW = w > 1 ? w >> 1 : 1;
    outH = h > 1 ? h >> 1 : 1;
    std::vector<uint8_t> dst(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
    const size_t stride = static_cast<size_t>(w) * 4;
    for (int y = 0; y < outH; ++y) {
        const int y0 = h > 1 ? 2 * y : 0;
        const int y1 = h > 1 ? 2 * y + 1 : 0;
        for (int x = 0; x < outW; ++x) {
            const size_t x0 = static_cast<size_t>(w > 1 ? 2 * x : 0) * 4;
            const size_t x1 = static_cast<size_t>(w > 1 ? 2 * x + 1 : 0) * 4;
            const uint8_t* p[4] = {
                rgba + static_cast<size_t>(y0) * stride + x0, rgba + static_cast<size_t>(y0) * stride + x1,
                rgba + static_cast<size_t>(y1) * stride + x0, rgba + static_cast<size_t>(y1) * stride + x1};
            uint32_t alphaSum = 0;
            uint32_t weighted[3] = {0, 0, 0};
            uint32_t plain[3] = {0, 0, 0};
            for (const uint8_t* s : p) {
                alphaSum += s[3];
                for (int c = 0; c < 3; ++c) {
                    weighted[c] += static_cast<uint32_t>(s[c]) * s[3];
                    plain[c] += s[c];
                }
            }
            uint8_t* d = dst.data() +
                         (static_cast<size_t>(y) * static_cast<size_t>(outW) + static_cast<size_t>(x)) * 4;
            for (int c = 0; c < 3; ++c) {
                d[c] = static_cast<uint8_t>(alphaSum == 0 ? divRoundHalfEven(plain[c], 4u)
                                                          : divRoundHalfEven(weighted[c], alphaSum));
            }
            d[3] = static_cast<uint8_t>(divRoundHalfEven(alphaSum, 4u));
        }
    }
    return dst;
}

std::vector<uint8_t> padToBlockMultiple(const uint8_t* rgba, int w, int h, int& paddedW, int& paddedH) {
    paddedW = (w + 3) / 4 * 4;
    paddedH = (h + 3) / 4 * 4;
    std::vector<uint8_t> out(static_cast<size_t>(paddedW) * static_cast<size_t>(paddedH) * 4);
    for (int y = 0; y < paddedH; ++y) {
        const uint8_t* srcRow = rgba + static_cast<size_t>(std::min(y, h - 1)) * static_cast<size_t>(w) * 4;
        uint8_t* dstRow = out.data() + static_cast<size_t>(y) * static_cast<size_t>(paddedW) * 4;
        std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
        for (int x = w; x < paddedW; ++x) {
            std::memcpy(dstRow + static_cast<size_t>(x) * 4, srcRow + static_cast<size_t>(w - 1) * 4, 4);
        }
    }
    return out;
}

void bleedTransparentTexels(uint8_t* rgba, int w, int h) {
    if (rgba == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    // 0 = not reached, 1 = has a colour (alpha > 0 or an earlier ring), 2 = in the ring being filled.
    std::vector<uint8_t> state(count, 0);
    bool anyTransparent = false;
    bool anySource = false;
    for (size_t i = 0; i < count; ++i) {
        if (rgba[i * 4 + 3] != 0) {
            state[i] = 1;
            anySource = true;
        } else {
            anyTransparent = true;
        }
    }
    if (!anyTransparent || !anySource) {
        return;
    }
    auto forNeighbours = [&](size_t i, auto&& fn) {
        const int x = static_cast<int>(i % static_cast<size_t>(w));
        const int y = static_cast<int>(i / static_cast<size_t>(w));
        for (int dy = -1; dy <= 1; ++dy) {
            const int ny = y + dy;
            if (ny < 0 || ny >= h) {
                continue;
            }
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx;
                if ((dx == 0 && dy == 0) || nx < 0 || nx >= w) {
                    continue;
                }
                fn(static_cast<size_t>(ny) * static_cast<size_t>(w) + static_cast<size_t>(nx));
            }
        }
    };
    std::vector<size_t> ring;
    for (size_t i = 0; i < count; ++i) {
        if (state[i] != 0) {
            continue;
        }
        bool touches = false;
        forNeighbours(i, [&](size_t n) { touches = touches || state[n] == 1; });
        if (touches) {
            state[i] = 2;
            ring.push_back(i);
        }
    }
    std::vector<std::array<uint8_t, 3>> colours;
    std::vector<size_t> next;
    while (!ring.empty()) {
        // Every colour of this ring comes from texels filled before it, so the order does not matter.
        colours.resize(ring.size());
        for (size_t k = 0; k < ring.size(); ++k) {
            uint32_t sum[3] = {0, 0, 0};
            uint32_t n = 0;
            forNeighbours(ring[k], [&](size_t j) {
                if (state[j] == 1) {
                    for (size_t c = 0; c < 3; ++c) {
                        sum[c] += rgba[j * 4 + c];
                    }
                    ++n;
                }
            });
            for (size_t c = 0; c < 3; ++c) {
                colours[k][c] = static_cast<uint8_t>(divRoundHalfEven(sum[c], n));
            }
        }
        for (size_t k = 0; k < ring.size(); ++k) {
            std::memcpy(rgba + ring[k] * 4, colours[k].data(), 3);
            state[ring[k]] = 1;
        }
        next.clear();
        for (const size_t i : ring) {
            forNeighbours(i, [&](size_t n) {
                if (state[n] == 0) {
                    state[n] = 2;
                    next.push_back(n);
                }
            });
        }
        ring.swap(next);
    }
}

std::vector<uint8_t> encodeDds(const uint8_t* rgba, int w, int h, BcFormat format, bool mipmaps) {
    if (rgba == nullptr || w <= 0 || h <= 0 || format == BcFormat::None) {
        return {};
    }
    ensureEncoders();
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<std::pair<int, int>> sizes;
    pixels.emplace_back(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    sizes.emplace_back(w, h);
    while (mipmaps && (sizes.back().first > 1 || sizes.back().second > 1)) {
        int nw = 0;
        int nh = 0;
        std::vector<uint8_t> level =
            downsampleMipLevel(pixels.back().data(), sizes.back().first, sizes.back().second, nw, nh);
        pixels.push_back(std::move(level));
        sizes.emplace_back(nw, nh);
    }
    return writeDdsFile(pixels, sizes, format, 1, 0, 0);
}

std::vector<uint8_t> encodePublishDds(const uint8_t* rgba, int w, int h, BcFormat format, int threads) {
    if (rgba == nullptr || w <= 0 || h <= 0 || format == BcFormat::None) {
        return {};
    }
    ensureEncoders();
    int pw = 0;
    int ph = 0;
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<std::pair<int, int>> sizes;
    pixels.push_back(padToBlockMultiple(rgba, w, h, pw, ph));
    sizes.emplace_back(pw, ph);
    bleedTransparentTexels(pixels.back().data(), pw, ph);
    while (sizes.back().first > 1 || sizes.back().second > 1) {
        int nw = 0;
        int nh = 0;
        std::vector<uint8_t> level =
            downsampleMipLevel(pixels.back().data(), sizes.back().first, sizes.back().second, nw, nh);
        bleedTransparentTexels(level.data(), nw, nh);
        pixels.push_back(std::move(level));
        sizes.emplace_back(nw, nh);
    }
    const bool padded = pw != w || ph != h;
    return writeDdsFile(pixels, sizes, format, threads, padded ? w : 0, padded ? h : 0);
}

std::string publishEncodeKey(const uint8_t* rgba, int w, int h, BcFormat format) {
    const size_t bytes = static_cast<size_t>(std::max(0, w)) * static_cast<size_t>(std::max(0, h)) * 4;
    const uint64_t hash =
        util::fnv1a64(std::string_view(reinterpret_cast<const char*>(rgba), rgba != nullptr ? bytes : 0));
    return std::string("xt-bc-cache 1 ") + bcFormatName(format) + " " + std::to_string(w) + "x" +
           std::to_string(h) + " rgba-fnv1a64=" + util::hex16(hash) + " encoder=" + kPublishEncoderVersion;
}

std::string EncodeCache::fileFor(const std::string& key) const {
    return detail::joinPath(m_dir, util::hex16(util::fnv1a64(key)) + ".xtbc");
}

bool EncodeCache::load(const std::string& key, std::vector<uint8_t>& out) const {
    if (!enabled()) {
        return false;
    }
    std::string file;
    if (!detail::readFile(fileFor(key), file) || file.size() < kCacheHeadBytes) {
        return false;
    }
    const auto* p = reinterpret_cast<const uint8_t*>(file.data());
    if (std::memcmp(p, "XTBC", 4) != 0 || texReadLe32(p + 4) != kCacheFormatVersion) {
        return false;
    }
    const size_t keyLength = texReadLe32(p + 8);
    if (keyLength != key.size() || file.size() < kCacheHeadBytes + keyLength + 16 ||
        file.compare(kCacheHeadBytes, keyLength, key) != 0) {
        return false;
    }
    const size_t at = kCacheHeadBytes + keyLength;
    const uint64_t payloadLength = texReadLe64(p + at);
    const uint64_t payloadHash = texReadLe64(p + at + 8);
    if (payloadLength != static_cast<uint64_t>(file.size() - at - 16)) {
        return false;
    }
    const std::string_view payload(file.data() + at + 16, static_cast<size_t>(payloadLength));
    if (util::fnv1a64(payload) != payloadHash) {
        return false;
    }
    out.assign(payload.begin(), payload.end());
    return true;
}

bool EncodeCache::store(const std::string& key, const std::vector<uint8_t>& payload) {
    if (!enabled()) {
        return false;
    }
    if (!m_madeDir) {
        std::error_code ec;
        std::filesystem::create_directories(compile::pathFromUtf8(m_dir), ec);
        m_madeDir = true;
    }
    std::string file;
    file.reserve(kCacheHeadBytes + key.size() + 16 + payload.size());
    file.append("XTBC", 4);
    std::vector<uint8_t> head;
    texWriteLe32(head, kCacheFormatVersion);
    texWriteLe32(head, static_cast<uint32_t>(key.size()));
    file.append(reinterpret_cast<const char*>(head.data()), head.size());
    file.append(key);
    head.clear();
    texWriteLe64(head, static_cast<uint64_t>(payload.size()));
    texWriteLe64(
        head, util::fnv1a64(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size())));
    file.append(reinterpret_cast<const char*>(head.data()), head.size());
    file.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    return detail::writeFile(fileFor(key), file);
}

}  // namespace xt::bundle
