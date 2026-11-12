#include "xt/gpu/image_texture.h"

#include <cstring>
#include <vector>

#include "xt/dds.h"

/// T-688 (Docs/design/50-c-abi.md §50.ae): see xt/gpu/image_texture.h. Moved out of
/// FrameBuilder::Impl::getOrCreateTexture (frame_builder.cpp) unchanged in order and effect; the builder
/// keeps its per-builder texture cache around it.

namespace xt::gpu {

ImageKey imageContentKey(const uint8_t* data, size_t bytes) {
    constexpr uint64_t kP1 = 11400714785074694791ull, kP2 = 14029467366897019727ull,
                       kP3 = 1609587929392839161ull, kP4 = 9650029242287828579ull;
    auto rotl = [](uint64_t v, int r) { return (v << r) | (v >> (64 - r)); };
    auto mixIn = [&](uint64_t acc, uint64_t w) { return rotl(acc + w * kP2, 31) * kP1; };
    uint64_t lane[4] = {kP1 + kP2, kP2, 0, 0 - kP1};
    size_t i = 0;
    for (; i + 32 <= bytes; i += 32) {
        for (size_t k = 0; k < 4; ++k) {
            uint64_t w = 0;
            std::memcpy(&w, data + i + k * 8u, 8);
            lane[k] = mixIn(lane[k], w);
        }
    }
    for (size_t k = 0; i < bytes; ++i, k = (k + 1) & 3u) {
        lane[k] = mixIn(lane[k], data[i]);
    }
    auto avalanche = [](uint64_t h) {
        h ^= h >> 33;
        h *= 14029467366897019727ull;
        h ^= h >> 29;
        h *= 1609587929392839161ull;
        return h ^ (h >> 32);
    };
    const auto length = static_cast<uint64_t>(bytes);
    ImageKey key;
    key.lo = avalanche(rotl(lane[0], 1) + rotl(lane[1], 7) + rotl(lane[2], 12) + rotl(lane[3], 18) + length);
    key.hi = avalanche((lane[0] * kP3) ^ (lane[1] * kP4) ^ rotl(lane[2], 23) ^ rotl(lane[3], 41) ^ ~length);
    return key;
}

namespace {

TextureFormat textureFormatOf(DdsFormat format) {
    return format == DdsFormat::Bc1   ? TextureFormat::Bc1
           : format == DdsFormat::Bc3 ? TextureFormat::Bc3
           : format == DdsFormat::Bc7 ? TextureFormat::Bc7
                                      : TextureFormat::Rgba8;
}

}  // namespace

ImageTextureResult acquireImageTexture(const ImageTextureRequest& r) {
    ImageTextureResult out;
    GpuDevice& device = *r.device;
    AssetResolver& assets = *r.assets;
    const IrView& ir = *r.ir;
    auto overLimit = [&r](int width, int height) {
        return r.maxTexture != 0 &&
               (static_cast<uint32_t>(width) > r.maxTexture || static_cast<uint32_t>(height) > r.maxTexture);
    };
    auto ok = [&out](TextureHandle tex, bool straight, const void* image, int width, int height,
                     int logicalWidth = 0, int logicalHeight = 0) {
        out.outcome = ImageTextureResult::Outcome::Ok;
        out.tex = tex;
        out.straight = straight;
        out.image = image;
        out.width = width;
        out.height = height;
        out.logicalWidth = logicalWidth;
        out.logicalHeight = logicalHeight;
        return out;
    };
    // T-657 (50 §50.ab item 2): on a sharing device, the content key the resolver kept from an earlier
    // upload of this image finds its shared texture without the pixels -- which may have been released (a
    // trimmed or hidden surface) and would have to be decoded again just to hash them. Not found: the
    // pixels are asked for below, as before.
    const bool share = device.sharesImageTextures();
    if (share) {
        AssetResolver::ImageContentKey kept;
        if (assets.retainedImageKey(r.asset, ir, r.scale, kept) && kept.width > 0 && kept.height > 0 &&
            kept.width <= 65535 && kept.height <= 65535 && !overLimit(kept.width, kept.height)) {
            if (TextureHandle found = device.findSharedImage(
                    ImageKey{kept.lo, kept.hi}, static_cast<uint16_t>(kept.width),
                    static_cast<uint16_t>(kept.height), textureFormatOf(kept.format))) {
                return ok(found, kept.format != DdsFormat::Rgba8, nullptr, kept.width, kept.height,
                          kept.logicalWidth, kept.logicalHeight);
            }
        }
    }
    // T-199 (23 §23.8): a block-compressed asset is uploaded as it is (straight alpha); a device that
    // cannot take the format returns 0 and the decoded pixels are used instead.
    if (const CompressedImage* compressed = assets.compressedImage(r.asset, ir, r.scale)) {
        bool isStraight = true;
        if (r.lookup != nullptr) {
            if (TextureHandle held =
                    r.lookup->find(compressed, compressed->width, compressed->height, isStraight)) {
                out.fromLookup = true;
                return ok(held, isStraight, compressed, compressed->width, compressed->height,
                          compressed->logicalWidth, compressed->logicalHeight);
            }
        }
        const TextureFormat fmt = compressed->format == DdsFormat::Bc1   ? TextureFormat::Bc1
                                  : compressed->format == DdsFormat::Bc3 ? TextureFormat::Bc3
                                                                         : TextureFormat::Bc7;
        if (compressed->width > 0 && compressed->height > 0 && compressed->width <= 65535 &&
            compressed->height <= 65535 && !overLimit(compressed->width, compressed->height) &&
            compressed->blocks.size() >=
                ddsLevelSize(compressed->format, compressed->width, compressed->height)) {
            const auto w = static_cast<uint16_t>(compressed->width);
            const auto h = static_cast<uint16_t>(compressed->height);
            // T-648: on a sharing device, one texture per content on the owner's devices.
            TextureHandle tex = 0;
            bool created = false;
            if (share) {
                const ImageKey key =
                    imageContentKey(compressed->blocks.data(),
                                    ddsLevelSize(compressed->format, compressed->width, compressed->height));
                tex = device.findSharedImage(key, w, h, fmt);
                if (tex == 0) {
                    tex =
                        device.createSharedImage(key, "xt image (bc)", w, h, fmt, compressed->blocks.data());
                    created = tex != 0;
                }
                if (tex != 0) {  // T-657: kept for the next builder that needs it (above).
                    assets.retainImageKey(
                        compressed,
                        AssetResolver::ImageContentKey{key.lo, key.hi, compressed->width, compressed->height,
                                                       compressed->format, compressed->logicalWidth,
                                                       compressed->logicalHeight});
                }
            } else {
                tex = device.createTexture("xt image (bc)", w, h, fmt, compressed->blocks.data());
                created = tex != 0;
            }
            if (tex != 0) {
                out.created = created;
                out.compressed = compressed;
                return ok(tex, true, compressed, compressed->width, compressed->height,
                          compressed->logicalWidth, compressed->logicalHeight);
            }
        }
    }
    // T-148: borrow the resolver's decoded image instead of copying it (T-199: the `@2x` variant at
    // dpr >= 1.5; an atlas page is one image shared by its assets).
    const ImageData* view = assets.imageView(r.asset, ir, r.scale);
    if (view == nullptr) {
        out.outcome = ImageTextureResult::Outcome::DecodeFailed;
        return out;
    }
    if (r.lookup != nullptr) {
        bool isStraight = false;
        if (TextureHandle held = r.lookup->find(view, view->width, view->height, isStraight)) {
            out.fromLookup = true;
            return ok(held, isStraight, view, view->width, view->height);
        }
    }
    const ImageData& image = *view;
    if (image.width <= 0 || image.height <= 0 || image.width > 65535 || image.height > 65535 ||
        image.rgba.size() < static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4) {
        out.outcome = ImageTextureResult::Outcome::BadSize;
        return out;
    }
    // Docs/tasks/T-263d.md part two: larger than the device says it can create; not handed to the device.
    if (overLimit(image.width, image.height)) {
        out.outcome = ImageTextureResult::Outcome::OverLimit;
        out.width = image.width;
        out.height = image.height;
        return out;
    }
    const size_t count = static_cast<size_t>(image.width) * static_cast<size_t>(image.height);
    const auto w = static_cast<uint16_t>(image.width);
    const auto h = static_cast<uint16_t>(image.height);
    // T-648: on a sharing device the texture of the same pixels may exist already (another surface of the
    // target uploaded it): no premultiply, no upload.
    ImageKey key;
    // T-657: the key, kept by the resolver for the next builder that needs it (above).
    auto retain = [&] {
        assets.retainImageKey(view, AssetResolver::ImageContentKey{key.lo, key.hi, image.width, image.height,
                                                                   DdsFormat::Rgba8});
    };
    if (share) {
        key = imageContentKey(image.rgba.data(), count * 4);
        if (TextureHandle found = device.findSharedImage(key, w, h, TextureFormat::Rgba8)) {
            retain();
            return ok(found, false, view, image.width, image.height);
        }
    }
    TextureHandle tex = 0;
    {
        // Premultiply on the CPU (gpu_frame.h: every texture is premultiplied). T-648: a scratch of this
        // upload only, freed with it.
        std::vector<uint8_t> premultiplied(count * 4);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* s = image.rgba.data() + i * 4;
            uint8_t* d = premultiplied.data() + i * 4;
            const uint32_t a = s[3];
            d[0] = static_cast<uint8_t>((s[0] * a + 127u) / 255u);
            d[1] = static_cast<uint8_t>((s[1] * a + 127u) / 255u);
            d[2] = static_cast<uint8_t>((s[2] * a + 127u) / 255u);
            d[3] = s[3];
        }
        tex = share ? device.createSharedImage(key, "xt image", w, h, TextureFormat::Rgba8,
                                               premultiplied.data())
                    : device.createTexture("xt image", w, h, TextureFormat::Rgba8, premultiplied.data());
    }
    if (tex == 0) {
        out.outcome = ImageTextureResult::Outcome::CreateFailed;
        return out;
    }
    if (share) {
        retain();
    }
    out.created = true;
    return ok(tex, false, view, image.width, image.height);
}

}  // namespace xt::gpu
