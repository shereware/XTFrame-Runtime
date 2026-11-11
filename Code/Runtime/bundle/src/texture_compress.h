#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// Block compression for pack() (T-199, Docs/design/26-layers-symbols-library.md §26.3.4,
/// 23 §23.8, 12 §12.7 `--compress`): RGBA8 -> BC1 / BC3 / BC7 blocks (vendored bc7enc / rgbcx,
/// Code/SDKs/bc7enc) written as a DDS file the runtime reads back through xt::parseDdsHeader.
/// T-687 (Docs/design/10-backends.md §10.13 rulings 2-6): the publish encode (padding to a multiple
/// of 4 with the 'XTLS' logical-size marker, the full mip chain, alpha bleed, threads) and the
/// encode cache. Internal to xt_bundle.

namespace xt::bundle {

/// The `--compress` choices.
enum class BcFormat : uint8_t { None = 0, Bc1 = 1, Bc3 = 2, Bc7 = 3 };

/// "none" | "bc1" | "bc3" | "bc7" (case-insensitive) -> format; false for anything else.
bool parseBcFormat(std::string_view name, BcFormat& out);
/// The manifest's `format` string: "bc1", "bc3", "bc7" ("png" is not this function's business).
const char* bcFormatName(BcFormat format);

/// True when any pixel of `rgba` (`w` x `h`, 4 bytes per pixel) has alpha < 255.
bool imageHasAlpha(const uint8_t* rgba, int w, int h);

/// Encodes `rgba` (straight alpha, row 0 = top) as a DDS file in `format` (never None), as given:
/// no padding, no bleed, no marker. With `mipmaps`, the full chain down to 1 x 1 (downsampleMipLevel)
/// follows level 0. Returns the file bytes; empty on a degenerate size. (The T-686 golden fixtures
/// are made with this; pack() uses encodePublishDds.)
std::vector<uint8_t> encodeDds(const uint8_t* rgba, int w, int h, BcFormat format, bool mipmaps);

/// T-687 (10 §10.13 ruling 5): under `auto`, an image (or atlas page) is block-compressed only when
/// both of its sides are at least this many pixels.
inline constexpr int kAutoCompressMinSide = 256;

/// T-687 (ruling 2): `rgba` (`w` x `h`) padded to the next multiple of 4 on each side by repeating
/// the last column / row; `paddedW` x `paddedH` receive the new size. A copy when already aligned.
std::vector<uint8_t> padToBlockMultiple(const uint8_t* rgba, int w, int h, int& paddedW, int& paddedH);

/// T-687 (ruling 4): gives every texel with alpha = 0 the colour of its nearest texels with alpha > 0,
/// in place: ring by ring (8-neighbourhood), each texel of the next ring takes the mean (rounded to
/// nearest, ties to even) of the RGB of its neighbours filled by earlier rings, until every texel is
/// reached. Alpha is never changed; an image without alpha = 0 texels, or with no texel above 0, is
/// left as it is.
void bleedTransparentTexels(uint8_t* rgba, int w, int h);

/// T-687 (ruling 3, 10 §10.11 "定案 7 定稿" ruling 1): the next mip level of `rgba` (`w` x `h`),
/// `max(1, w >> 1)` x `max(1, h >> 1)` (written to `outW` x `outH`). Each texel is the 2 x 2 block
/// (2x, 2y)..(2x+1, 2y+1) (a 1-texel axis takes its one texel twice; an odd last row / column is not
/// sampled): alpha = sum(a) / 4, colour = sum(c * a) / sum(a), both rounded to nearest with ties to
/// even; where sum(a) = 0 the colour is sum(c) / 4 (same rounding).
std::vector<uint8_t> downsampleMipLevel(const uint8_t* rgba, int w, int h, int& outW, int& outH);

/// T-687 (rulings 2-4): the DDS file pack() writes for a bitmap: `rgba` (`w` x `h`, straight alpha)
/// padded to a multiple of 4 (padToBlockMultiple), level 0 bled (bleedTransparentTexels), every
/// further level made from the bled level before it (downsampleMipLevel) and bled in turn, the full
/// chain down to 1 x 1, encoded in `format` (never None). The header carries the padded size and,
/// when padding happened, the logical-size marker (`dwReserved1[0..2]` = 'XTLS', `w`, `h`). Blocks are
/// encoded on up to `threads` threads (< 1 = 1); the bytes do not depend on the thread count.
std::vector<uint8_t> encodePublishDds(const uint8_t* rgba, int w, int h, BcFormat format, int threads);

/// T-687 (ruling 6): the encoder version in the cache key -- the vendored encoder's commit plus the
/// version of the padding / bleed / chain / parameter pipeline above. Change it whenever any of those
/// changes what encodePublishDds() writes.
extern const char* const kPublishEncoderVersion;

/// T-687 (ruling 6): the cache key of encodePublishDds(`rgba`, `w`, `h`, `format`): the content hash
/// of the pixels (FNV-1a 64 over the RGBA bytes) with their size, the format and
/// kPublishEncoderVersion, as one line of text.
std::string publishEncodeKey(const uint8_t* rgba, int w, int h, BcFormat format);

/// T-687 (ruling 6): the encode cache, one file per key in a directory:
/// `<hex16(FNV-1a 64 of the key)>.xtbc` = "XTBC", format version (u32), key length (u32), key bytes,
/// payload length (u64), FNV-1a 64 of the payload (u64), payload (the DDS file). A file that is
/// missing, short, of another format version, for another key or whose payload hash does not match
/// is a miss. Writes go to a temporary name first and are moved into place. Use from one thread at a
/// time.
class EncodeCache {
public:
    /// `dir` = the cache directory (created on the first store); empty = no cache.
    explicit EncodeCache(std::string dir = std::string()) : m_dir(std::move(dir)) {}
    bool enabled() const { return !m_dir.empty(); }
    const std::string& dir() const { return m_dir; }
    /// The cached payload for `key` in `out`; false on a miss.
    bool load(const std::string& key, std::vector<uint8_t>& out) const;
    /// Stores `payload` under `key`; false when it could not be written (the pack goes on).
    bool store(const std::string& key, const std::vector<uint8_t>& payload);
    /// The file (UTF-8 path) a key lives in.
    std::string fileFor(const std::string& key) const;

private:
    std::string m_dir;
    bool m_madeDir = false;
};

}  // namespace xt::bundle
