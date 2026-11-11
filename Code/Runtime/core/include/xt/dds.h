#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/// DDS container reading (T-199, Docs/design/26-layers-symbols-library.md §26.3.4, 23 §23.8):
/// the header of a `.dds` file as `xtframe-pack --compress` writes it (BC1 / BC3 / BC7, top-down,
/// optional mip chain) or as a plain RGBA8 DDS. Only the header is interpreted here; block
/// decompression lives in xt_sw (software path) and the GPU device uploads the blocks as they are.
/// xt_core stays third-party free, so this is a hand-written parser of the documented layout
/// (124-byte DDS_HEADER, optional 20-byte DDS_HEADER_DXT10).

namespace xt {

/// Pixel formats a DDS file can carry into XTFrame.
enum class DdsFormat : uint8_t {
    Unknown = 0,
    /// BC1 (DXT1): 8-byte 4x4 blocks, opaque or 1-bit alpha.
    Bc1 = 1,
    /// BC3 (DXT5): 16-byte 4x4 blocks, interpolated alpha.
    Bc3 = 2,
    /// BC7 (BPTC UNORM): 16-byte 4x4 blocks.
    Bc7 = 3,
    /// Uncompressed 32-bit RGBA (8 bits each, R first in memory).
    Rgba8 = 4,
};

/// What parseDdsHeader() reads.
struct DdsInfo {
    int width = 0;
    int height = 0;
    /// Number of mip levels stored (at least 1).
    int mipCount = 1;
    DdsFormat format = DdsFormat::Unknown;
    /// Byte offset of level 0's data inside the file.
    size_t dataOffset = 0;
    /// Byte size of level 0's data.
    size_t level0Size = 0;
    /// T-686 (Docs/design/10-backends.md §10.13 ruling 2): the image's logical size. A packer that
    /// padded the image to a multiple of 4 writes `DDS_HEADER.dwReserved1[0..2]` = kDdsLogicalSizeTag,
    /// logical width, logical height; `width` x `height` stay the padded (stored) size. Equal to
    /// `width` x `height` when the file carries no valid marker.
    int logicalWidth = 0;
    int logicalHeight = 0;
    /// T-686: a marker was present but its values were out of range (0, larger than the stored size,
    /// or 4 or more texels smaller): ignored, the logical size is the stored size.
    bool logicalRejected = false;
};

/// T-686: 'XTLS' as a little-endian u32, the tag of the logical-size marker in
/// `DDS_HEADER.dwReserved1[0]`.
inline constexpr uint32_t kDdsLogicalSizeTag = 0x534C5458u;

/// True when `bytes` starts with the DDS magic ("DDS ").
bool isDds(const uint8_t* bytes, size_t size);

/// Parses the header of a DDS file. False when the magic, the header or the pixel format is not
/// one of `DdsFormat`, or when the file is shorter than level 0 needs.
bool parseDdsHeader(const uint8_t* bytes, size_t size, DdsInfo& out);

/// Bytes per 4x4 block of a compressed format (8 for BC1, 16 for BC3/BC7); 0 for the others.
size_t ddsBlockBytes(DdsFormat format);

/// Byte size of one `width` x `height` level in `format` (blocks rounded up; 4 bytes per texel for
/// Rgba8).
size_t ddsLevelSize(DdsFormat format, int width, int height);

/// T-686: a whole single-level DDS file (DX10 header) holding `size` bytes of `data` as one
/// `width` x `height` level in `format` (Bc1 / Bc3 / Bc7 / Rgba8); with `logicalWidth` > 0 the
/// logical-size marker (kDdsLogicalSizeTag) is written too. How FileAssetResolver hands one stored
/// level of a DDS it keeps to its decode function (which takes whole files). Empty when `size` is
/// smaller than the level or the format is Unknown.
std::vector<uint8_t> makeDdsLevelFile(DdsFormat format, int width, int height, const uint8_t* data,
                                      size_t size, int logicalWidth = 0, int logicalHeight = 0);

}  // namespace xt
