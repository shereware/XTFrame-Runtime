#include "sw_dds.h"

#include <cstring>
#include <mutex>
#include <vector>

#include "bc7decomp.h"
#include "rgbcx.h"
#include "xt/dds.h"

/// See sw_dds.h. Blocks are decoded 4x4 at a time into a scratch tile and copied into the image
/// (clipped at the right/bottom edges of a size that is not a multiple of 4).

namespace xt::sw {

namespace {

/// One-time rgbcx table setup (its decoders read the BC1 interpolation tables). T-652: once per
/// process under std::call_once, because bundles may decode on different threads at the same time
/// (50 §50.1); the tables are read-only afterwards.
void ensureRgbcx() {
    static std::once_flag once;
    std::call_once(once, [] { rgbcx::init(); });
}

/// T-686 (Docs/design/10-backends.md §10.13 ruling 2): the decoded stored level 0 cut to the
/// logical size the header's marker gives (a no-op without one).
ImageData cropToLogical(ImageData image, const DdsInfo& info) {
    if (info.logicalWidth == info.width && info.logicalHeight == info.height) {
        return image;
    }
    ImageData cropped;
    cropped.width = info.logicalWidth;
    cropped.height = info.logicalHeight;
    const size_t row = static_cast<size_t>(info.logicalWidth) * 4;
    const size_t stride = static_cast<size_t>(info.width) * 4;
    cropped.rgba.resize(row * static_cast<size_t>(info.logicalHeight));
    for (size_t y = 0; y < static_cast<size_t>(info.logicalHeight); ++y) {
        std::memcpy(cropped.rgba.data() + y * row, image.rgba.data() + y * stride, row);
    }
    return cropped;
}

}  // namespace

bool decodeDdsBytes(const uint8_t* bytes, size_t size, ImageData& out) {
    DdsInfo info;
    if (!parseDdsHeader(bytes, size, info)) {
        return false;
    }
    ImageData result;
    result.width = info.width;
    result.height = info.height;
    const size_t w = static_cast<size_t>(info.width);
    const size_t h = static_cast<size_t>(info.height);
    result.rgba.assign(w * h * 4, 0);
    const uint8_t* data = bytes + info.dataOffset;
    if (info.format == DdsFormat::Rgba8) {
        std::memcpy(result.rgba.data(), data, w * h * 4);
        out = cropToLogical(std::move(result), info);
        return true;
    }
    ensureRgbcx();
    const size_t blockBytes = ddsBlockBytes(info.format);
    const size_t blocksX = (w + 3) / 4;
    const size_t blocksY = (h + 3) / 4;
    uint8_t tile[16 * 4];
    for (size_t by = 0; by < blocksY; ++by) {
        for (size_t bx = 0; bx < blocksX; ++bx) {
            const uint8_t* block = data + (by * blocksX + bx) * blockBytes;
            switch (info.format) {
                case DdsFormat::Bc1:
                    rgbcx::unpack_bc1(block, tile, true);
                    break;
                case DdsFormat::Bc3:
                    rgbcx::unpack_bc3(block, tile);
                    break;
                case DdsFormat::Bc7:
                    bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(tile));
                    break;
                default:
                    return false;
            }
            for (size_t ty = 0; ty < 4; ++ty) {
                const size_t y = by * 4 + ty;
                if (y >= h) {
                    break;
                }
                for (size_t tx = 0; tx < 4; ++tx) {
                    const size_t x = bx * 4 + tx;
                    if (x >= w) {
                        break;
                    }
                    std::memcpy(result.rgba.data() + (y * w + x) * 4, tile + (ty * 4 + tx) * 4, 4);
                }
            }
        }
    }
    out = cropToLogical(std::move(result), info);
    return true;
}

}  // namespace xt::sw
