#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "file_open_utf8.h"
#include "xt/asset_resolver.h"
#include "xt/dds.h"

/// FileAssetResolver: reads assets from a root directory. Uses <cstdio> rather than <fstream> so
/// this file stays trivially compatible with xt_core's -fno-exceptions build (docs/design/14-coding-
/// standards.md §14.1). See docs/design/10-backends.md §10.4, docs/tasks/T-12.md "AssetResolver".
///
/// T-199 (docs/design/26-layers-symbols-library.md §26.3.4, 22 §22.10): atlas assets
/// (`atlas:<n>#<asset>`) decode their whole page once and share it, `@2x` variants are picked at
/// dpr >= 1.5, and a DDS file keeps its blocks for the GPU next to the decoded pixels.

namespace xt {

FileAssetResolver::FileAssetResolver(std::string rootDir, ImageDecodeFn decode)
    : FileAssetResolver(std::vector<std::string>{std::move(rootDir)}, decode) {}

FileAssetResolver::FileAssetResolver(std::vector<std::string> rootDirs, ImageDecodeFn decode)
    : m_rootDirs(std::move(rootDirs)), m_decode(decode) {}

namespace {

/// Reads the whole file at `path` (UTF-8, T-261e) into `out`; false (leaving `out` unchanged) if
/// it cannot be opened or read.
bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    std::FILE* file = openFileUtf8(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return false;
    }
    long size = std::ftell(file);
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    size_t readCount = bytes.empty() ? 0 : std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (readCount != bytes.size()) {
        return false;
    }
    out = std::move(bytes);
    return true;
}

uint32_t readBe16(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 8) | p[1]; }
uint32_t readBe32(const uint8_t* p) { return (readBe16(p) << 16) | readBe16(p + 2); }
uint32_t readLe16(const uint8_t* p) { return (static_cast<uint32_t>(p[1]) << 8) | p[0]; }
uint32_t readLe32(const uint8_t* p) { return (readLe16(p + 2) << 16) | readLe16(p); }

/// T-160: an image's pixel size from its file header alone -- PNG (IHDR), JPEG (first SOFn
/// segment), GIF (logical screen), BMP (DIB header) and, T-199, DDS -- the dimensions the
/// stb_image-based decoder reports for the same bytes. False for any other/truncated input (the
/// caller then decodes).
bool imageSizeFromHeader(std::span<const uint8_t> b, int& width, int& height) {
    const size_t n = b.size();
    static const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 24 && std::equal(kPng, kPng + 8, b.data()) && b[12] == 'I' && b[13] == 'H' && b[14] == 'D' &&
        b[15] == 'R') {
        width = static_cast<int>(readBe32(&b[16]));
        height = static_cast<int>(readBe32(&b[20]));
        return width > 0 && height > 0;
    }
    if (n >= 4 && b[0] == 0xFF && b[1] == 0xD8) {
        size_t i = 2;
        while (i + 1 < n) {
            if (b[i] != 0xFF) {
                return false;  // Not at a marker: malformed, let the decoder decide.
            }
            uint8_t marker = b[i + 1];
            if (marker == 0xFF) {
                ++i;  // Fill byte.
                continue;
            }
            i += 2;
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
                continue;  // Standalone markers carry no length.
            }
            if (marker == 0xD9 || marker == 0xDA || i + 2 > n) {
                return false;  // EOI / start of scan before any SOF, or truncated.
            }
            uint32_t length = readBe16(&b[i]);
            bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (sof) {
                if (length < 7 || i + 7 > n) {
                    return false;
                }
                height = static_cast<int>(readBe16(&b[i + 3]));
                width = static_cast<int>(readBe16(&b[i + 5]));
                return width > 0 && height > 0;
            }
            if (length < 2) {
                return false;
            }
            i += length;
        }
        return false;
    }
    if (n >= 10 && b[0] == 'G' && b[1] == 'I' && b[2] == 'F' && b[3] == '8' && (b[4] == '7' || b[4] == '9') &&
        b[5] == 'a') {
        width = static_cast<int>(readLe16(&b[6]));
        height = static_cast<int>(readLe16(&b[8]));
        return width > 0 && height > 0;
    }
    if (n >= 26 && b[0] == 'B' && b[1] == 'M') {
        uint32_t dibSize = readLe32(&b[14]);
        if (dibSize == 12) {
            width = static_cast<int>(readLe16(&b[18]));
            height = static_cast<int>(readLe16(&b[20]));
        } else if (dibSize >= 40) {
            width = static_cast<int32_t>(readLe32(&b[18]));
            int32_t h = static_cast<int32_t>(readLe32(&b[22]));
            height = h < 0 ? -h : h;  // Negative = top-down rows; the decoded size is |h|.
        } else {
            return false;
        }
        return width > 0 && height > 0;
    }
    DdsInfo dds;
    if (parseDdsHeader(b.data(), n, dds)) {
        width = dds.width;
        height = dds.height;
        return true;
    }
    return false;
}

/// `<stem><suffix><ext>` of a path (`ext` = from the last '.' after the last separator; no
/// extension: the suffix is appended); `suffix` is "@2x" or (T-345) "@3x".
std::string variantName(std::string_view name, std::string_view suffix) {
    size_t dot = name.rfind('.');
    size_t slash = name.find_last_of("/\\");
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return std::string(name) + std::string(suffix);
    }
    return std::string(name.substr(0, dot)) + std::string(suffix) + std::string(name.substr(dot));
}

/// T-345 (25 §25.4.2 item 4): the variant a scale (`ui-scale` factor x dpr) asks for: 2 = `@3x`
/// at >= 2.5, 1 = `@2x` at >= 1.5, else 0.
int variantFor(float scale) { return scale >= 2.5f ? 2 : scale >= 1.5f ? 1 : 0; }

constexpr size_t kFailed = static_cast<size_t>(-1);

/// FNV-1a of an embedded asset's bytes: with their size, what the image is (T-215, T-263c B).
uint64_t embeddedBytesHash(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (uint8_t byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ull;
    }
    return hash;
}

}  // namespace

bool FileAssetResolver::readBytes(std::string_view name, std::vector<uint8_t>& out) {
    for (const std::string& root : m_rootDirs) {
        std::string path = root;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path += '/';
        }
        path += name;
        if (readWholeFile(path, out)) {
            return true;
        }
    }
    return false;
}

// --- T-150: external textures -----------------------------------------------------------------

bool isExternalAssetName(std::string_view name) {
    return name.size() >= kExternalAssetScheme.size() &&
           name.substr(0, kExternalAssetScheme.size()) == kExternalAssetScheme;
}

std::string_view externalAssetName(std::string_view name) {
    return isExternalAssetName(name) ? name.substr(kExternalAssetScheme.size()) : name;
}

bool isExternalAsset(AssetId asset, const IrView& view) {
    std::span<const IrAsset> assets = view.assets().assets();
    size_t index = static_cast<size_t>(asset);
    return index < assets.size() && isExternalAssetName(view.string(assets[index].name));
}

void AssetResolver::setExternalTexture(std::string_view name, uint32_t texture, int width, int height,
                                       bool straightAlpha) {
    ExternalTexture entry;
    entry.texture = texture;
    entry.width = width;
    entry.height = height;
    entry.straightAlpha = straightAlpha;
    for (auto& [key, value] : m_external) {
        if (key == name) {
            entry.pixels = value.pixels;  // T-191: kept across a texture rebind.
            value = entry;
            return;
        }
    }
    m_external.emplace_back(std::string(name), entry);
}

void AssetResolver::setExternalPixels(std::string_view name, const ImageData* pixels) {
    for (auto& [key, value] : m_external) {
        if (key == name) {
            value.pixels = pixels;
            if (pixels != nullptr) {
                value.width = pixels->width;
                value.height = pixels->height;
            }
            return;
        }
    }
    ExternalTexture entry;
    entry.pixels = pixels;
    if (pixels != nullptr) {
        entry.width = pixels->width;
        entry.height = pixels->height;
    }
    m_external.emplace_back(std::string(name), entry);
}

void AssetResolver::clearExternalTexture(std::string_view name) {
    for (size_t i = 0; i < m_external.size(); ++i) {
        if (m_external[i].first == name) {
            m_external.erase(m_external.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

const AssetResolver::ExternalTexture* AssetResolver::externalTexture(std::string_view name) const {
    for (const auto& [key, value] : m_external) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

const AssetResolver::ExternalTexture* AssetResolver::externalTexture(AssetId asset,
                                                                     const IrView& view) const {
    std::span<const IrAsset> assets = view.assets().assets();
    size_t index = static_cast<size_t>(asset);
    if (index >= assets.size()) {
        return nullptr;
    }
    std::string_view name = view.string(assets[index].name);
    if (!isExternalAssetName(name)) {
        return nullptr;
    }
    return externalTexture(externalAssetName(name));
}

// --- T-199: atlas names and image metadata -----------------------------------------------------

bool isAtlasAssetName(std::string_view name) {
    return name.size() > kAtlasAssetScheme.size() &&
           name.substr(0, kAtlasAssetScheme.size()) == kAtlasAssetScheme &&
           name.find('#') != std::string_view::npos;
}

std::string_view atlasAssetPage(std::string_view name) {
    if (!isAtlasAssetName(name)) {
        return std::string_view();
    }
    size_t hash = name.find('#');
    return name.substr(kAtlasAssetScheme.size(), hash - kAtlasAssetScheme.size());
}

std::string_view atlasAssetSource(std::string_view name) {
    if (!isAtlasAssetName(name)) {
        return name;
    }
    return name.substr(name.find('#') + 1);
}

bool isAtlasAsset(AssetId asset, const IrView& view) {
    std::span<const IrAsset> assets = view.assets().assets();
    size_t index = static_cast<size_t>(asset);
    return index < assets.size() && isAtlasAssetName(view.string(assets[index].name));
}

void AssetResolver::setImageMeta(std::string_view name, ImageMeta meta) {
    for (auto& [key, value] : m_imageMeta) {
        if (key == name) {
            value = std::move(meta);
            return;
        }
    }
    m_imageMeta.emplace_back(std::string(name), std::move(meta));
}

void AssetResolver::clearImageMeta(std::string_view name) {
    for (size_t i = 0; i < m_imageMeta.size(); ++i) {
        if (m_imageMeta[i].first == name) {
            m_imageMeta.erase(m_imageMeta.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

const AssetResolver::ImageMeta* AssetResolver::imageMeta(std::string_view name) const {
    for (const auto& [key, value] : m_imageMeta) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

const AssetResolver::ImageMeta* AssetResolver::imageMeta(AssetId asset, const IrView& view) const {
    std::span<const IrAsset> assets = view.assets().assets();
    size_t index = static_cast<size_t>(asset);
    if (index >= assets.size() || m_imageMeta.empty()) {
        return nullptr;
    }
    return imageMeta(view.string(assets[index].name));
}

Rect AssetResolver::imageSubUv(AssetId asset, const IrView& view) const {
    const ImageMeta* meta = imageMeta(asset, view);
    if (meta == nullptr || !meta->inAtlas) {
        return Rect{0, 0, 1, 1};
    }
    return meta->subUv;
}

const ImageData* AssetResolver::imageView(AssetId asset, const IrView& view) {
    if (!decodeImage(asset, view, m_viewScratch)) {
        return nullptr;
    }
    return &m_viewScratch;
}

// --- FileAssetResolver decode cache ------------------------------------------------------------

bool FileAssetResolver::readAssetBytes(AssetId asset, const IrView& view, int variant,
                                       std::vector<uint8_t>& out, std::string& fileKey) {
    const bool hiDpi = variant != 0;
    std::span<const IrAsset> assets = view.assets().assets();
    size_t index = static_cast<size_t>(asset);
    if (index >= assets.size()) {
        return false;
    }
    const IrAsset& irAsset = assets[index];
    std::string_view name = view.string(irAsset.name);
    if (irAsset.embedded == 0 && isExternalAssetName(name)) {
        return false;  // T-150: an `engine://` asset has no bytes anywhere; never look on disk.
    }
    if (irAsset.embedded != 0) {
        if (hiDpi) {
            return false;  // Embedded bytes have no variant.
        }
        std::span<const uint8_t> bytes = view.assets().data(irAsset);
        out.assign(bytes.begin(), bytes.end());
        // T-215: keyed by content (FNV-1a + size), so two IRs that embed the same image share
        // one decode, and (T-263g ruling 10) a decode on the idle list is found again by the
        // same bytes in a recompiled IR. The key starts with the byte 0x01 (split from "embedded"
        // so the hex escape does not swallow the 'e'), which no file path does.
        fileKey =
            "\x01"
            "embedded:" +
            std::to_string(bytes.size()) + ":" + std::to_string(embeddedBytesHash(bytes));
        ++m_embeddedHashes;
        return true;
    }
    const ImageMeta* meta = imageMeta(name);
    if (isAtlasAssetName(name)) {
        // The whole page; the cell comes from the ImageMeta (imageSubUv).
        std::vector<std::string> candidates;
        if (variant == 2) {
            return false;  // T-345: atlas pages have no 3x file; the caller falls back to 2x.
        }
        if (meta != nullptr && meta->inAtlas) {
            if (hiDpi) {
                if (meta->atlasFile2x.empty()) {
                    return false;
                }
                candidates.push_back(meta->atlasFile2x);
            } else {
                candidates.push_back(meta->atlasFile);
            }
        } else {
            const std::string page = "atlas/" + std::string(atlasAssetPage(name)) + (hiDpi ? "@2x" : "");
            candidates.push_back(page + ".png");
            candidates.push_back(page + ".dds");
        }
        for (const std::string& candidate : candidates) {
            if (readBytes(candidate, out)) {
                fileKey = candidate;
                return true;
            }
        }
        return false;
    }
    if (!hiDpi) {
        fileKey = std::string(name);
        return readBytes(name, out);
    }
    const std::string& listed = meta == nullptr ? std::string()
                                : variant == 2  ? meta->variant3x
                                                : meta->variant2x;
    if (!listed.empty()) {
        fileKey = listed;
        if (readBytes(listed, out)) {
            return true;
        }
    }
    fileKey = variantName(name, variant == 2 ? "@3x" : "@2x");
    return readBytes(fileKey, out);
}

void FileAssetResolver::decodeInto(const std::vector<uint8_t>& bytes, Entry& entry) {
    entry.ok = false;
    entry.hasCompressed = false;
    if (m_decode == nullptr) {
        return;
    }
    DdsInfo dds;
    if (parseDdsHeader(bytes.data(), bytes.size(), dds) && dds.format != DdsFormat::Rgba8 &&
        dds.format != DdsFormat::Unknown) {
        entry.compressed.width = dds.width;
        entry.compressed.height = dds.height;
        entry.compressed.format = dds.format;
        entry.compressed.blocks.assign(
            bytes.begin() + static_cast<std::ptrdiff_t>(dds.dataOffset),
            bytes.begin() + static_cast<std::ptrdiff_t>(dds.dataOffset + dds.level0Size));
        entry.hasCompressed = true;
    }
    entry.ok = m_decode(bytes.data(), bytes.size(), entry.image);
    if (!entry.ok) {
        entry.hasCompressed = false;
        entry.compressed.blocks.clear();
    }
}

FileAssetResolver::Slot* FileAssetResolver::slotFor(AssetId asset, const IrView& view, int variant) {
    std::span<const IrAsset> assets = view.assets().assets();
    const size_t index = static_cast<size_t>(asset);
    if (index >= assets.size()) {
        return nullptr;  // T-255: an id outside the ASST table names nothing.
    }
    const IrAsset& irAsset = assets[index];
    if (irAsset.embedded != 0) {
        // No file name to go by: the slot is found by the bytes' place in their IR blob, and is
        // the asset's only while those bytes hash to what the slot recorded.
        const std::span<const uint8_t> bytes = view.assets().data(irAsset);
        std::vector<EmbeddedSlot>& slots = m_embedded[variant];
        auto it = std::lower_bound(slots.begin(), slots.end(), bytes.data(),
                                   [](const EmbeddedSlot& s, const uint8_t* data) {
                                       return std::less<const uint8_t*>()(s.data, data);
                                   });
        const bool found = it != slots.end() && it->data == bytes.data();
        const uint32_t serial = view.loadSerial();
        if (found && it->size == bytes.size()) {
            for (uint8_t i = 0; i < it->serialCount; ++i) {
                if (it->serials[i] != serial) {
                    continue;
                }
                // A load that checked the bytes: they are the same ones. Most recently used
                // first (T-263g ruling 9), so the view that was not used for longest is the
                // one a fifth view replaces.
                for (; i > 0; --i) {
                    it->serials[i] = it->serials[i - 1];
                }
                it->serials[0] = serial;
                return &it->slot;
            }
        }
        // docs/tasks/T-263c.md B: first use by this load of the IR. A reload frees the old blob
        // and the new one may sit at the same address with the same size and another image
        // inside, so the address alone proves nothing; the bytes are hashed once per load.
        const uint64_t hash = embeddedBytesHash(bytes);
        ++m_embeddedHashes;
        if (!found) {
            it = slots.insert(it, EmbeddedSlot{bytes.data(), bytes.size(), {serial}, 1, hash, Slot{}});
        } else {
            if (it->size != bytes.size() || it->hash != hash) {
                it->slot = Slot{};  // Another image now lives at this address: nobody has seen it.
                it->serialCount = 0;
            }
            it->size = bytes.size();
            it->hash = hash;
            const uint8_t kept = static_cast<uint8_t>(
                std::min<size_t>(it->serialCount, kVerifiedSerials - 1));  // The least recently used goes.
            for (uint8_t i = kept; i > 0; --i) {
                it->serials[i] = it->serials[i - 1];
            }
            it->serials[0] = serial;
            it->serialCount = static_cast<uint8_t>(kept + 1);
        }
        return &it->slot;
    }
    const std::string_view name = view.string(irAsset.name);
    std::vector<NamedSlot>& slots = m_named[variant];
    auto it = std::lower_bound(slots.begin(), slots.end(), name, [](const NamedSlot& s, std::string_view n) {
        return std::string_view(s.name) < n;
    });
    if (it == slots.end() || it->name != name) {
        it = slots.insert(it, NamedSlot{std::string(name), Slot{}, {}, false});
    }
    // docs/tasks/T-263g.md ruling 8: held by every live source from its first lookup of the name
    // (once per source; the per-frame lookups after that find the serial and allocate nothing).
    // A slot nobody held was waiting with its idle decode: that decode is live again.
    const uint32_t serial = view.loadSerial();
    if (std::find(it->sources.begin(), it->sources.end(), serial) == it->sources.end()) {
        it->sources.push_back(serial);
        it->lapsed = false;
        if (it->slot.entry != 0 && it->slot.entry != kFailed) {
            reviveEntry(it->slot.entry - 1);
        }
    }
    return &it->slot;
}

const FileAssetResolver::Entry* FileAssetResolver::ensureDecoded(AssetId asset, const IrView& view,
                                                                 int variant) {
    // docs/tasks/T-263b.md A6: looked up by what `view` says the asset is, never by the bare id.
    Slot* resolved = slotFor(asset, view, variant);
    if (resolved == nullptr) {
        return nullptr;
    }
    if (resolved->entry == 0) {
        std::vector<uint8_t> bytes;
        std::string fileKey;
        size_t slot = kFailed;
        if (readAssetBytes(asset, view, variant, bytes, fileKey)) {
            // An atlas page (or any file) already decoded for another asset is shared.
            if (!fileKey.empty()) {
                for (const FileEntry& file : m_fileEntry) {
                    if (file.key == fileKey) {
                        slot = file.entry + 1;
                        reviveEntry(file.entry);  // T-263g: an idle decode found again is live.
                        break;
                    }
                }
            }
            if (slot == kFailed) {
                // T-263e: an entry releaseSource() emptied is used again before the deque grows.
                size_t entryIndex = m_entries.size();
                if (!m_freeEntries.empty()) {
                    entryIndex = m_freeEntries.back();
                    m_freeEntries.pop_back();
                } else {
                    m_entries.emplace_back();
                }
                Entry& entry = m_entries[entryIndex];
                decodeInto(bytes, entry);
                if (!fileKey.empty()) {
                    FileEntry file;
                    file.key = fileKey;
                    file.entry = entryIndex;
                    // T-263c ruling 5: what revalidate() compares the disk with. Once per
                    // decoded file; embedded bytes (their key starts with \x01) are not a file.
                    file.hasStamp = fileKey.front() != '\x01' && fileStamp(fileKey, file.size, file.mtime);
                    m_fileEntry.push_back(std::move(file));
                }
                slot = entryIndex + 1;
            }
        }
        resolved->entry = slot;  // readAssetBytes()/decodeInto() add no slot: still valid.
    }
    const size_t slot = resolved->entry;
    if (slot == kFailed) {
        return variant > 0 ? ensureDecoded(asset, view, variant - 1) : nullptr;
    }
    const Entry& entry = m_entries[slot - 1];
    if (!entry.ok) {
        return variant > 0 ? ensureDecoded(asset, view, variant - 1) : nullptr;
    }
    return &entry;
}

void FileAssetResolver::resetAssetIds() {
    // docs/tasks/T-263f.md item 13: an embedded slot is the only way to its decode once the slots
    // are gone -- releaseSource() of the IR finds nothing inside the blob any more -- so the
    // decodes go with the slots, by the routine releaseSource() uses. docs/tasks/T-263g.md
    // rulings 8 and 10: with the named slots gone too, no live source holds any decode, so every
    // entry in use goes that way: onto the idle list, where the next lookup of its file path or
    // of the same embedded bytes finds it, or freed when nothing could (a file forgetFile() or
    // revalidate() took the key of, a failed decode), or when the budget says so.
    for (std::vector<EmbeddedSlot>& slots : m_embedded) {
        slots.clear();
    }
    for (std::vector<NamedSlot>& slots : m_named) {
        slots.clear();
    }
    std::vector<size_t> dropped;
    for (size_t index = 0; index < m_entries.size(); ++index) {
        if (!m_entries[index].idle &&
            std::find(m_freeEntries.begin(), m_freeEntries.end(), index) == m_freeEntries.end()) {
            dropped.push_back(index + 1);
        }
    }
    releaseEntries(dropped);
}

void FileAssetResolver::releaseSource(const IrView& view) {
    std::vector<size_t> dropped;  // `Slot::entry` values (index + 1) the dropped slots resolved to
    // docs/tasks/T-263g.md ruling 8: `view` lets go of every named slot it held. One nobody holds
    // any more stays, with its decode, for the idle list; one with nothing decoded (never
    // decoded beyond its header size, or failed) has nothing to keep and goes.
    const uint32_t serial = view.loadSerial();
    for (std::vector<NamedSlot>& slots : m_named) {
        for (size_t i = 0; i < slots.size();) {
            NamedSlot& named = slots[i];
            auto held = std::find(named.sources.begin(), named.sources.end(), serial);
            if (held == named.sources.end()) {
                ++i;
                continue;
            }
            named.sources.erase(held);
            if (!named.sources.empty()) {
                ++i;
                continue;
            }
            const size_t entry = named.slot.entry;
            if (entry == 0 || entry == kFailed) {
                slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (std::find(dropped.begin(), dropped.end(), entry) == dropped.end()) {
                dropped.push_back(entry);
            }
            ++i;
        }
    }
    // docs/tasks/T-263e.md must-fix 1. Only addresses are compared; `view`'s bytes are not read.
    const IrBlob blob = view.blob();
    if (blob.data == nullptr || blob.size == 0) {
        releaseEntries(dropped);
        return;
    }
    const uint8_t* const begin = blob.data;
    const uint8_t* const end = blob.data + blob.size;
    auto before = [](const EmbeddedSlot& s, const uint8_t* data) {
        return std::less<const uint8_t*>()(s.data, data);
    };
    for (std::vector<EmbeddedSlot>& slots : m_embedded) {
        // Sorted by address: the blob's slots are one run.
        const auto first = std::lower_bound(slots.begin(), slots.end(), begin, before);
        const auto last = std::lower_bound(first, slots.end(), end, before);
        for (auto it = first; it != last; ++it) {
            const size_t entry = it->slot.entry;
            if (entry != 0 && entry != kFailed &&
                std::find(dropped.begin(), dropped.end(), entry) == dropped.end()) {
                dropped.push_back(entry);
            }
        }
        slots.erase(first, last);
    }
    releaseEntries(dropped);
}

bool FileAssetResolver::entryRetained(size_t resolved) const {
    for (int variant = 0; variant < 2; ++variant) {
        // An embedded slot exists only while its IR is live (T-263e); a named one is held while
        // it has a source.
        for (const EmbeddedSlot& s : m_embedded[variant]) {
            if (s.slot.entry == resolved) {
                return true;
            }
        }
        for (const NamedSlot& s : m_named[variant]) {
            if (s.slot.entry == resolved && !s.sources.empty()) {
                return true;
            }
        }
    }
    return false;
}

void FileAssetResolver::releaseEntries(const std::vector<size_t>& dropped) {
    for (const size_t resolved : dropped) {
        if (entryRetained(resolved)) {
            // Another IR that embeds the same image, or another live source of the file (an
            // atlas page's other cells), shares the decode and keeps it live. A named slot that
            // just lost its last source has nothing to wait for then: its name may never be
            // used again, and a lookup of it reads the file and finds the decode by its path.
            for (std::vector<NamedSlot>& slots : m_named) {
                std::erase_if(slots, [resolved](const NamedSlot& s) {
                    return s.slot.entry == resolved && s.sources.empty();
                });
            }
            continue;
        }
        idleEntry(resolved - 1);
    }
    trimIdle();
}

void FileAssetResolver::idleEntry(size_t index) {
    Entry& entry = m_entries[index];
    if (entry.idle) {
        return;
    }
    // The slots that lapsed while another one had the decode live go now (see NamedSlot::lapsed).
    for (std::vector<NamedSlot>& slots : m_named) {
        std::erase_if(slots, [index](const NamedSlot& s) {
            return s.slot.entry == index + 1 && s.sources.empty() && s.lapsed;
        });
    }
    // Findable: by its file path or, docs/tasks/T-263g.md ruling 10, its embedded content key, or
    // through a source-less named slot.
    bool findable = false;
    for (const FileEntry& file : m_fileEntry) {
        findable = findable || file.entry == index;
    }
    for (int variant = 0; variant < 2 && !findable; ++variant) {
        for (const NamedSlot& s : m_named[variant]) {
            findable = findable || s.slot.entry == index + 1;
        }
    }
    const size_t bytes = entry.image.rgba.capacity() + entry.compressed.blocks.capacity();
    if (!entry.ok || !findable || m_idleBudget == 0 || bytes > m_idleBudget) {
        freeEntry(index);
        return;
    }
    entry.idle = true;
    m_idle.push_back(IdleDecode{index, bytes});
    m_idleBytes += bytes;
}

void FileAssetResolver::reviveEntry(size_t index) {
    Entry& entry = m_entries[index];
    if (!entry.idle) {
        return;
    }
    entry.idle = false;
    for (size_t i = 0; i < m_idle.size(); ++i) {
        if (m_idle[i].entry == index) {
            m_idleBytes -= m_idle[i].bytes;
            m_idle.erase(m_idle.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    // The other source-less slots of this decode (an atlas page's other cells) wait on: the
    // source that took it back may look them up next. Should it not, they go with the decode's
    // next idle spell instead of piling up.
    for (std::vector<NamedSlot>& slots : m_named) {
        for (NamedSlot& s : slots) {
            if (s.slot.entry == index + 1 && s.sources.empty()) {
                s.lapsed = true;
            }
        }
    }
}

void FileAssetResolver::freeEntry(size_t index) {
    m_entries[index] = Entry{};  // Move-assigned from an empty one: the pixels are freed.
    for (size_t i = 0; i < m_fileEntry.size();) {
        if (m_fileEntry[i].entry == index) {
            m_fileEntry.erase(m_fileEntry.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (std::vector<NamedSlot>& slots : m_named) {
        std::erase_if(slots,
                      [index](const NamedSlot& s) { return s.slot.entry == index + 1 && s.sources.empty(); });
    }
    m_freeEntries.push_back(index);
}

void FileAssetResolver::trimIdle() {
    size_t freed = 0;
    while (freed < m_idle.size() && (m_idleBudget == 0 || m_idleBytes > m_idleBudget)) {
        const IdleDecode& oldest = m_idle[freed];
        m_idleBytes -= oldest.bytes;
        freeEntry(oldest.entry);  // Resets `idle` with the rest of the entry.
        ++freed;
    }
    m_idle.erase(m_idle.begin(), m_idle.begin() + static_cast<std::ptrdiff_t>(freed));
}

void FileAssetResolver::forgetFile(std::string_view file) {
    for (size_t i = 0; i < m_fileEntry.size(); ++i) {
        if (m_fileEntry[i].key != file) {
            continue;
        }
        const size_t index = m_fileEntry[i].entry;
        if (m_entries[index].idle) {
            // T-263g: no live source uses it, and nothing may find it again: freed at once.
            reviveEntry(index);
            freeEntry(index);
            return;
        }
        // Only the file-path lookup goes; the entry itself stays in `m_entries` (pointers into
        // it may still be in use).
        m_fileEntry.erase(m_fileEntry.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void FileAssetResolver::setFileStatusFn(FileStatusFn status) { m_fileStatus = status; }

void FileAssetResolver::setIdleDecodeBudget(size_t bytes) {
    m_idleBudget = bytes;
    trimIdle();
}

bool FileAssetResolver::fileStamp(std::string_view name, uint64_t& size, uint64_t& mtime) const {
    // docs/tasks/T-263e.md ruling B: the core asks the file system nothing itself.
    if (m_fileStatus == nullptr) {
        return false;
    }
    for (const std::string& root : m_rootDirs) {  // The order readBytes() tries them in.
        std::string path = root;
        if (!path.empty() && path.back() != '/' && path.back() != '\\') {
            path += '/';
        }
        path += name;
        // The path is UTF-8, as readWholeFile() reads it (T-261e).
        if (m_fileStatus(path, size, mtime)) {
            return true;
        }
    }
    return false;
}

size_t FileAssetResolver::revalidate() {
    if (m_fileStatus == nullptr) {
        return 0;  // T-263e: nothing to compare the cached files with.
    }
    size_t discarded = 0;
    for (size_t i = 0; i < m_fileEntry.size();) {
        const FileEntry& file = m_fileEntry[i];
        uint64_t size = 0;
        uint64_t mtime = 0;
        if (!file.hasStamp ||
            (fileStamp(file.key, size, mtime) && size == file.size && mtime == file.mtime)) {
            ++i;
            continue;
        }
        // Rewritten, replaced by a file in a nearer root, or gone: the next asset that names it
        // reads it again. As in forgetFile() the decoded entry itself stays (pointers into it
        // may be in use); unlike there, the assets that resolved to it are looked up afresh.
        const size_t index = file.entry;
        const size_t resolved = index + 1;
        if (m_entries[index].idle) {
            // T-263g: no live source uses it: freed at once, with its key and waiting slots
            // (`file` is gone with the key; `i` now names the next one).
            reviveEntry(index);
            freeEntry(index);
            ++discarded;
            continue;
        }
        for (std::vector<NamedSlot>& slots : m_named) {
            for (size_t n = 0; n < slots.size();) {
                NamedSlot& named = slots[n];
                if (named.slot.entry != resolved) {
                    ++n;
                } else if (named.sources.empty()) {
                    // T-263g: waiting for the old decode with no source of its own: nothing left
                    // to wait for.
                    slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(n));
                } else {
                    named.slot = Slot{};
                    ++n;
                }
            }
        }
        m_fileEntry.erase(m_fileEntry.begin() + static_cast<std::ptrdiff_t>(i));
        ++discarded;
    }
    // A size read from the header of a file nobody decoded yet (T-160) has no stamp to compare:
    // it is simply read again.
    for (NamedSlot& named : m_named[0]) {
        if (named.slot.entry == 0) {
            named.slot.headerOk = false;
        }
    }
    return discarded;
}

bool FileAssetResolver::decodeImage(AssetId asset, const IrView& view, ImageData& out) {
    const Entry* entry = ensureDecoded(asset, view, 0);
    if (entry == nullptr) {
        return false;
    }
    out = entry->image;
    return true;
}

const ImageData* FileAssetResolver::imageView(AssetId asset, const IrView& view) {
    const Entry* entry = ensureDecoded(asset, view, 0);
    return entry != nullptr ? &entry->image : nullptr;
}

const ImageData* FileAssetResolver::imageView(AssetId asset, const IrView& view, float dpr) {
    const Entry* entry = ensureDecoded(asset, view, variantFor(dpr));
    return entry != nullptr ? &entry->image : nullptr;
}

const CompressedImage* FileAssetResolver::compressedImage(AssetId asset, const IrView& view, float dpr) {
    const Entry* entry = ensureDecoded(asset, view, variantFor(dpr));
    return entry != nullptr && entry->hasCompressed ? &entry->compressed : nullptr;
}

Vec2 FileAssetResolver::imageSize(AssetId asset, const IrView& view) {
    // T-199: an atlas cell's size is the manifest's, whatever the page measures.
    const ImageMeta* meta = imageMeta(asset, view);
    if (meta != nullptr && meta->inAtlas) {
        return Vec2{static_cast<float>(meta->w), static_cast<float>(meta->h)};
    }
    // T-160: an image nobody has decoded yet is sized from its header (see the declaration); once
    // decoded, the decoded image stays the authority.
    // T-263b A6: cached with the asset's slot (its name or embedded bytes), not under its id.
    Slot* slot = slotFor(asset, view, false);
    const bool decoded = slot != nullptr && slot->entry != 0;
    if (!decoded) {
        if (slot != nullptr && slot->headerOk) {
            return slot->headerSize;
        }
        if (slot != nullptr && m_decode != nullptr) {
            const IrAsset& irAsset = view.assets().assets()[static_cast<size_t>(asset)];
            int w = 0, h = 0;
            bool ok = false;
            if (irAsset.embedded != 0) {
                ok = imageSizeFromHeader(view.assets().data(irAsset), w, h);
            } else if (!isExternalAssetName(view.string(irAsset.name))) {
                std::vector<uint8_t> bytes;
                std::string fileKey;
                ok = readAssetBytes(asset, view, false, bytes, fileKey) && imageSizeFromHeader(bytes, w, h);
            }
            if (ok) {
                slot->headerSize = Vec2{static_cast<float>(w), static_cast<float>(h)};
                slot->headerOk = true;
                return slot->headerSize;
            }
        }
    }
    const ImageData* data = imageView(asset, view);
    if (data == nullptr) {
        return Vec2{0, 0};
    }
    return Vec2{static_cast<float>(data->width), static_cast<float>(data->height)};
}

}  // namespace xt
