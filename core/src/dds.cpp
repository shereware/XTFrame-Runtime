#include "xt/dds.h"

#include <cstring>

/// See xt/dds.h. Layout reference: the DDS_HEADER / DDS_PIXELFORMAT / DDS_HEADER_DXT10 structures
/// documented for the DDS programming guide (all fields little-endian u32).

namespace xt {

namespace {

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint32_t kDdpfFourCc = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kDdsdMipmapCount = 0x20000;

constexpr uint32_t fourCc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

// DXGI_FORMAT values of the DX10 extension header.
constexpr uint32_t kDxgiR8G8B8A8Unorm = 28;
constexpr uint32_t kDxgiBc1Unorm = 71;
constexpr uint32_t kDxgiBc3Unorm = 77;
constexpr uint32_t kDxgiBc7Unorm = 98;

}  // namespace

bool isDds(const uint8_t* bytes, size_t size) {
    return bytes != nullptr && size >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' &&
           bytes[3] == ' ';
}

size_t ddsBlockBytes(DdsFormat format) {
    switch (format) {
        case DdsFormat::Bc1:
            return 8;
        case DdsFormat::Bc3:
        case DdsFormat::Bc7:
            return 16;
        default:
            return 0;
    }
}

size_t ddsLevelSize(DdsFormat format, int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    const size_t w = static_cast<size_t>(width);
    const size_t h = static_cast<size_t>(height);
    if (format == DdsFormat::Rgba8) {
        return w * h * 4;
    }
    const size_t block = ddsBlockBytes(format);
    if (block == 0) {
        return 0;
    }
    return ((w + 3) / 4) * ((h + 3) / 4) * block;
}

bool parseDdsHeader(const uint8_t* bytes, size_t size, DdsInfo& out) {
    // Magic (4) + DDS_HEADER (124).
    if (!isDds(bytes, size) || size < 128) {
        return false;
    }
    const uint8_t* h = bytes + 4;
    if (readLe32(h) != 124) {
        return false;
    }
    const uint32_t flags = readLe32(h + 4);
    const uint32_t height = readLe32(h + 8);
    const uint32_t width = readLe32(h + 12);
    const uint32_t mipCount = (flags & kDdsdMipmapCount) != 0 ? readLe32(h + 24) : 1;
    // DDS_PIXELFORMAT at offset 72 of the header: size, flags, fourCC, bitCount, masks.
    const uint8_t* pf = h + 72;
    if (readLe32(pf) != 32) {
        return false;
    }
    const uint32_t pfFlags = readLe32(pf + 4);
    const uint32_t code = readLe32(pf + 8);
    DdsInfo info;
    info.width = static_cast<int>(width);
    info.height = static_cast<int>(height);
    info.mipCount = mipCount == 0 ? 1 : static_cast<int>(mipCount);
    info.dataOffset = 128;
    if ((pfFlags & kDdpfFourCc) != 0 && code == fourCc('D', 'X', '1', '0')) {
        if (size < 148) {
            return false;
        }
        const uint32_t dxgi = readLe32(bytes + 128);
        switch (dxgi) {
            case kDxgiBc1Unorm:
                info.format = DdsFormat::Bc1;
                break;
            case kDxgiBc3Unorm:
                info.format = DdsFormat::Bc3;
                break;
            case kDxgiBc7Unorm:
                info.format = DdsFormat::Bc7;
                break;
            case kDxgiR8G8B8A8Unorm:
                info.format = DdsFormat::Rgba8;
                break;
            default:
                return false;
        }
        info.dataOffset = 148;
    } else if ((pfFlags & kDdpfFourCc) != 0) {
        if (code == fourCc('D', 'X', 'T', '1')) {
            info.format = DdsFormat::Bc1;
        } else if (code == fourCc('D', 'X', 'T', '5')) {
            info.format = DdsFormat::Bc3;
        } else {
            return false;
        }
    } else if ((pfFlags & kDdpfRgb) != 0 && readLe32(pf + 12) == 32 && readLe32(pf + 16) == 0x000000FFu &&
               readLe32(pf + 20) == 0x0000FF00u && readLe32(pf + 24) == 0x00FF0000u) {
        info.format = DdsFormat::Rgba8;
    } else {
        return false;
    }
    if (info.width <= 0 || info.height <= 0) {
        return false;
    }
    info.level0Size = ddsLevelSize(info.format, info.width, info.height);
    if (info.level0Size == 0 || info.dataOffset + info.level0Size > size) {
        return false;
    }
    out = info;
    return true;
}

}  // namespace xt
