#pragma once

#include <cstddef>

#include "xt/asset_resolver.h"

/// The test-only view into FileAssetResolver (its `friend`), shared by the xt_tests files that
/// count its slots, decodes and I/O (test_context_reload.cpp, test_file_asset_resolver.cpp).

namespace xt {

/// T-263e: what a FileAssetResolver holds (Docs/tasks/T-263e.md must-fix 1).
struct FileAssetResolverTestAccess {
    /// Every asset lookup slot: named and embedded, both variants.
    static size_t slotCount(const FileAssetResolver& r) {
        return r.m_named[0].size() + r.m_named[1].size() + r.m_embedded[0].size() + r.m_embedded[1].size();
    }
    static size_t embeddedSlotCount(const FileAssetResolver& r) {
        return r.m_embedded[0].size() + r.m_embedded[1].size();
    }
    /// The bytes of every decoded image (and compressed block set) the resolver keeps.
    static size_t decodedBytes(const FileAssetResolver& r) {
        size_t bytes = 0;
        for (const FileAssetResolver::Entry& entry : r.m_entries) {
            bytes += entry.image.rgba.capacity() + entry.compressed.blocks.capacity();
        }
        return bytes;
    }
    static size_t entryCount(const FileAssetResolver& r) { return r.m_entries.size(); }
    static size_t fileKeyCount(const FileAssetResolver& r) { return r.m_fileEntry.size(); }
    /// T-263g: the idle list -- decodes no live source uses, and the bytes they count for.
    static size_t idleCount(const FileAssetResolver& r) { return r.m_idle.size(); }
    static size_t idleBytes(const FileAssetResolver& r) { return r.m_idleBytes; }
    /// T-263g ruling 9: how often embedded bytes were hashed so far.
    static size_t embeddedHashCount(const FileAssetResolver& r) { return r.m_embeddedHashes; }
    /// T-452: how often readBytes() read a whole file, and how often a file status was asked.
    static size_t readBytesCount(const FileAssetResolver& r) { return r.m_readBytesCalls; }
    static size_t statCount(const FileAssetResolver& r) { return r.m_statCalls; }
    /// T-452: header-only sizes kept across resetAssetIds().
    static size_t headerSizeCount(const FileAssetResolver& r) { return r.m_headerSizes.size(); }
};

}  // namespace xt
