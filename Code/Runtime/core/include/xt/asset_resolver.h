#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xt/dds.h"
#include "xt/ir.h"
#include "xt/types.h"

/// AssetResolver: the interface between xt_core / a backend and the host's asset storage. See
/// Docs/design/10-backends.md §10.4, Docs/tasks/T-12.md "AssetResolver".
///
/// `decodeImage`'s stb_image-based implementation cannot live here (xt_core stays third-party
/// free); xt_sw provides `xt::sw::decodeImageBytes` and callers wire it into FileAssetResolver's
/// constructor (see Docs/tasks/T-12.md).

namespace xt {

/// Decoded image, always RGBA8, non-premultiplied, tightly packed (stride == width * 4).
struct ImageData {
    /// Image width in pixels.
    int width = 0;
    /// Image height in pixels.
    int height = 0;
    /// RGBA8 pixel data, `width * height * 4` bytes.
    std::vector<uint8_t> rgba;
};

/// T-199 (Docs/design/26-layers-symbols-library.md §26.3.4, 23 §23.8): an image kept in a
/// block-compressed form (a DDS file `xtframe-pack --compress` wrote), for a GPU backend that can
/// upload it as is. Straight (non-premultiplied) alpha, row 0 = top, level 0 only.
struct CompressedImage {
    int width = 0;
    int height = 0;
    /// Bc1, Bc3 or Bc7 (never Rgba8/Unknown).
    DdsFormat format = DdsFormat::Unknown;
    /// The 4x4 blocks of level 0, `ddsLevelSize(format, width, height)` bytes.
    std::vector<uint8_t> blocks;
};

/// Host-provided asset storage. `decodeImage`/`imageSize` need the `IrView` that referenced the
/// asset because embedded assets' bytes live in that view's asset chunk.
class AssetResolver {
public:
    /// Polymorphic base; instances are owned via a pointer and destroyed through it.
    virtual ~AssetResolver() = default;

    /// Reads a non-embedded, name-referenced asset's raw bytes. Returns false on failure, leaving
    /// `out` unchanged.
    virtual bool readBytes(std::string_view name, std::vector<uint8_t>& out) = 0;

    /// Decodes an image asset to RGBA8. Returns false on failure (unknown asset id, read failure,
    /// or unsupported/corrupt image data), leaving `out` unchanged.
    virtual bool decodeImage(AssetId asset, const IrView& view, ImageData& out) = 0;

    /// Convenience: the decoded image's size, or (0, 0) on failure.
    virtual Vec2 imageSize(AssetId asset, const IrView& view) = 0;

    /// Borrowed view of the decoded image, valid until the next call that may evict it
    /// (invalidate/reload). Default: decodes into an internal scratch via decodeImage() and returns
    /// a pointer to it (one copy, same as before). Overrides return the cache entry directly.
    /// Returns nullptr on failure (same conditions as decodeImage). T-148.
    /// The default implementation's pointer is additionally invalidated by the next imageView()
    /// call on the same resolver.
    virtual const ImageData* imageView(AssetId asset, const IrView& view);

    /// T-199 (26 §26.3.4 `@2x`): like imageView(asset, view), but at a device pixel ratio
    /// `dpr >= 1.5` a resolver that has the asset's `@2x` variant (`<stem>@2x<ext>` beside the
    /// file, a bundle manifest `variants.2x`, or an atlas page's 2x file) returns that variant
    /// instead -- twice the pixels, same intrinsic size (imageSize() never changes with dpr) and
    /// the same 0..1 uv rectangles. T-345 (25 §25.4.2 item 4): `dpr` is the element's `ui-scale`
    /// factor x the device pixel ratio, and at `dpr >= 2.5` the `@3x` variant (`<stem>@3x<ext>`,
    /// manifest `variants.3x`; atlas pages have none) is preferred, then `@2x`. Default: the 1x
    /// image.
    virtual const ImageData* imageView(AssetId asset, const IrView& view, float dpr) {
        (void)dpr;
        return imageView(asset, view);
    }

    /// T-199 (23 §23.8): the block-compressed form of the asset, when the resolver holds one (a
    /// DDS file), chosen like imageView(asset, view, dpr); nullptr when the asset is not
    /// compressed or cannot be read. A GPU backend prefers this over the decoded pixels; the
    /// software backend always uses imageView(). Valid as long as imageView()'s pointer would be.
    virtual const CompressedImage* compressedImage(AssetId asset, const IrView& view, float dpr) {
        (void)asset;
        (void)view;
        (void)dpr;
        return nullptr;
    }

    /// T-263e: the IR `view` indexes is about to be dropped -- its Document unloaded, or replaced
    /// by a reload / hot reload -- so a resolver that keeps anything per IR (FileAssetResolver:
    /// the lookup slots and decodes of the embedded assets, whose bytes live in that blob) lets go
    /// of it. Context calls it on its ContextConfig::assets for every Document it drops
    /// (unloadDocument(), reloadDocument() and hotReload() when the new Document is installed,
    /// the deferred forms of both when the outermost dispatching call returns), `view` still
    /// readable during the call; not when the Context itself is destroyed. T-263f: `~Context`
    /// does not call it, so a host that keeps one resolver across several Contexts unloads a
    /// Context's Documents before destroying that Context, or calls resetAssetIds() on the
    /// resolver (FileAssetResolver) once it is gone. T-263g: FileAssetResolver also ends the
    /// IR's hold on the decodes of the files it names (see its override). A host that opens
    /// IrViews of its own and hands them to a resolver calls it the same way before freeing such a
    /// blob. Every imageView() / compressedImage() pointer obtained through `view` is invalid
    /// afterwards. The core does not know the renderer: a host with a gpu::FrameBuilder that
    /// wants the dropped IR's textures gone at once forwards the call to
    /// FrameBuilder::invalidateAllAssets() (otherwise the builder drops them when a later source
    /// takes the IR's place, see FrameBuilder::build). Default: nothing to release.
    virtual void releaseSource(const IrView& view) { (void)view; }

    /// T-199 (26 §26.3.4, 22 §22.10 `atlas[]`): what a bundle manifest knows about an image
    /// asset -- its manifest nine-slice and, for an asset packed into an atlas page, the cell it
    /// occupies. Set by `Bundle::open` (and by tests); a resolver without an entry treats the
    /// asset as a whole, unsliced image.
    struct ImageMeta {
        /// `manifest.assets[].manifest.slice` (L, R, T, B, source pixels): the default of
        /// `border-image-slice: auto`.
        bool hasSlice = false;
        float slice[4] = {0, 0, 0, 0};
        /// True when the asset lives in an atlas page: `atlasFile` (relative to the resolver's
        /// root, e.g. "atlas/0.png") holds it at the cell `x, y, w, h` (pixels) of the
        /// `atlasW` x `atlasH` page; `subUv` is that cell in 0..1.
        bool inAtlas = false;
        std::string atlasFile;
        /// The page's 2x variant ("atlas/0@2x.png", same layout at twice the size), or empty.
        std::string atlasFile2x;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int atlasW = 0;
        int atlasH = 0;
        Rect subUv{0, 0, 1, 1};
        /// A non-atlas asset's 2x file (manifest `variants.2x`), or empty (the `@2x` naming
        /// convention is still tried).
        std::string variant2x;
        /// T-345 (25 §25.4.2 item 4): the 3x file (manifest `variants.3x`), or empty (the `@3x`
        /// naming convention is still tried).
        std::string variant3x;
    };

    /// Records `meta` for the asset named `name` (the ASST name as the IR spells it, e.g.
    /// "assets/<hash>.png" or "atlas:0#assets/<hash>.png"); replaces an existing entry.
    void setImageMeta(std::string_view name, ImageMeta meta);
    /// Removes the entry of `name` (no-op when absent).
    void clearImageMeta(std::string_view name);
    /// The entry of `name`, or nullptr. Valid until the next setImageMeta/clearImageMeta call.
    const ImageMeta* imageMeta(std::string_view name) const;
    /// The entry of `asset` (looked up by its ASST name), or nullptr.
    const ImageMeta* imageMeta(AssetId asset, const IrView& view) const;
    /// The 0..1 rectangle of `asset` inside the image imageView() returns: its atlas cell when it
    /// is atlased, else the whole image {0, 0, 1, 1}.
    Rect imageSubUv(AssetId asset, const IrView& view) const;

    /// External (host-provided) textures, Docs/tasks/T-150.md. An `<img src="engine://<name>">`
    /// is never decoded: the host binds `name` to a texture it owns on the GPU backend, and this
    /// registry (kept on the resolver so layout and the GPU FrameBuilder read one table) supplies
    /// both the intrinsic size the layout uses and the texture the FrameBuilder samples.
    struct ExternalTexture {
        /// The backend's texture handle (`xt::gpu::TextureHandle`, i.e. the value
        /// `GpuDevice::createTexture` would issue; a GL texture name for xt_gl). 0 = size only:
        /// layout works, nothing is drawn.
        uint32_t texture = 0;
        /// Size in pixels, reported as the image's intrinsic size.
        int width = 0;
        int height = 0;
        /// true: the texture holds straight (non-premultiplied) alpha; the GPU backend
        /// premultiplies while sampling. false: premultiplied, like every texture xt uploads.
        bool straightAlpha = false;
        /// T-191: CPU pixels of the same content (RGBA8, straight alpha, like ImageData always
        /// is), borrowed from the host, for software renderers -- the SW backend draws these where
        /// it would otherwise draw nothing. nullptr = none. Set by setExternalPixels().
        const ImageData* pixels = nullptr;
    };

    /// Binds `name` (without the `engine://` prefix) to `texture` of `width` x `height`;
    /// replaces an existing binding. Call it before the first layout of a document that uses
    /// the name (a later change updates the size only at the next layout pass). The resolver
    /// never destroys `texture`.
    void setExternalTexture(std::string_view name, uint32_t texture, int width, int height,
                            bool straightAlpha = false);
    /// T-191: sets the CPU pixels of `name`'s binding (creating a size-only binding with texture 0
    /// when there is none); the size becomes `pixels`' size. `pixels` is borrowed and must stay
    /// valid until the next set/clear call for `name`; nullptr removes the pixels only. A later
    /// setExternalTexture() keeps them.
    void setExternalPixels(std::string_view name, const ImageData* pixels);
    /// Removes the binding of `name` (no-op when absent). The image then draws nothing and
    /// measures 0 x 0.
    void clearExternalTexture(std::string_view name);
    /// The binding of `name`, or nullptr. The pointer is valid until the next set/clear call.
    const ExternalTexture* externalTexture(std::string_view name) const;
    /// The binding of an `engine://<name>` asset, or nullptr when `asset` is not an external
    /// asset or `name` is not bound.
    const ExternalTexture* externalTexture(AssetId asset, const IrView& view) const;

private:
    // Scratch for the default imageView(); unused by resolvers that override it.
    ImageData m_viewScratch;
    // T-150: `name` -> binding, unsorted (a handful of entries at most).
    std::vector<std::pair<std::string, ExternalTexture>> m_external;
    // T-199: `name` -> ImageMeta, unsorted; read at load/resolve time, never per frame.
    std::vector<std::pair<std::string, ImageMeta>> m_imageMeta;
};

/// A decode function matching xt::sw::decodeImageBytes's signature: decodes `size` bytes at
/// `bytes` (an encoded image file's contents) into `out`. Returns false on failure.
using ImageDecodeFn = bool (*)(const uint8_t* bytes, size_t size, ImageData& out);

/// T-263e: a file-status function, injected into FileAssetResolver like its decode function
/// (xt_core itself asks the file system nothing: no `<filesystem>`, no `stat`). Reports the size
/// in bytes and the last-write time -- an opaque tick count, only compared for equality -- of the
/// regular file at `utf8Path` (UTF-8, as every path of the resolver); false when there is no such
/// file. `xt::fileStatusUtf8` ("xt/bundle/bundle.h", xt_bundle) is the stock implementation.
using FileStatusFn = bool (*)(std::string_view utf8Path, uint64_t& size, uint64_t& modifiedTicks);

/// AssetResolver backed by one or more root directories on disk. `readBytes` reads the file at
/// `root/name` for the first root (in order) where that file can be opened -- nearer roots win
/// (Docs/design/22-bundle.md §22.2, Docs/design/13-api.md, T-138).
/// `decodeImage` reads an embedded asset's bytes directly from the IrView's asset chunk, or falls
/// back to `readBytes(name)` for a non-embedded asset; either way the bytes are decoded via the
/// `decode` function pointer injected at construction (nullptr means "cannot decode", i.e. every
/// decodeImage call fails). Results are cached per asset name -- per embedded byte range for an
/// embedded asset -- never per bare AssetId (T-263b: ids restart at 0 in every IR, and one
/// resolver serves every Document of a Context or bundle), including failures, so a broken asset
/// is not re-decoded on every call. T-261e: root directories and asset names are UTF-8; on
/// Windows the files are opened through the wide API. T-261f: a path that is not valid UTF-8
/// is treated as missing.
class FileAssetResolver : public AssetResolver {
public:
    /// `decode` is used for every image decode; pass nullptr if decoding is not needed/available.
    FileAssetResolver(std::string rootDir, ImageDecodeFn decode);
    /// Multi-root form (T-138): `readBytes` tries `rootDirs[0]/name`, `rootDirs[1]/name`, ... in
    /// order. The single-root constructor delegates here with a one-element list.
    FileAssetResolver(std::vector<std::string> rootDirs, ImageDecodeFn decode);
    /// T-528 (49 §49.3, 2026-10-29 ruling): appends `dir` to the roots `readBytes()` tries, after
    /// the existing ones (the implied symbol group's directory, as `xtframe-pack` adds it to the
    /// compile's search directories); a root already in the list is not added again. A name looked
    /// up and not found before the call stays unresolved until resetAssetIds().
    void addSearchDir(std::string dir);

    /// See AssetResolver::readBytes; reads `root/name` from the first root that has it.
    bool readBytes(std::string_view name, std::vector<uint8_t>& out) override;
    /// See AssetResolver::decodeImage.
    bool decodeImage(AssetId asset, const IrView& view, ImageData& out) override;
    /// See AssetResolver::imageSize. T-160: when the asset has not been decoded yet, the size is
    /// read from the PNG/JPEG/GIF/BMP header (cached like the decode) instead of decoding the whole
    /// image, so layout's intrinsic-size query no longer pays for the decode; the decode happens
    /// when the renderer first asks for the pixels. Other formats still decode here.
    Vec2 imageSize(AssetId asset, const IrView& view) override;
    /// Zero-copy: returns the address of the cached decode result for `asset` (decoding it on
    /// first use), or nullptr when decoding fails. T-263g: the pointer is valid until
    /// releaseSource() of the IR it was asked through, or resetAssetIds(), whichever comes first;
    /// after either call every pointer obtained before it counts as invalid, for a named (file)
    /// asset and an embedded one alike -- the decode may have been freed, or kept on the idle list
    /// and handed out again at this or another address (see setIdleDecodeBudget()). Ask again.
    const ImageData* imageView(AssetId asset, const IrView& view) override;
    /// T-199: `dpr >= 1.5` selects the `@2x` variant, T-345 `dpr >= 2.5` the `@3x` one (falling
    /// back to `@2x`, then 1x; see AssetResolver::imageView(dpr)): the
    /// manifest's `variants.2x` / the atlas page's 2x file when an ImageMeta names one, else the
    /// naming convention `<stem>@2x<ext>` next to the file; a missing or undecodable variant falls
    /// back to the 1x image. Both variants are cached independently (and kept or let go
    /// independently, see setIdleDecodeBudget()).
    /// The pixels of an atlas asset are its whole page (one decode shared by every asset of the
    /// page: the returned pointer is the same for all of them); imageSubUv() gives the cell.
    const ImageData* imageView(AssetId asset, const IrView& view, float dpr) override;
    /// T-199: the DDS blocks of an asset whose file (or `@2x` file) is a DDS (see
    /// AssetResolver::compressedImage); nullptr for any other file format.
    const CompressedImage* compressedImage(AssetId asset, const IrView& view, float dpr) override;

    /// T-215: forgets every per-asset lookup (which decoded image each asset name or embedded
    /// byte range maps to, and the header-only sizes) for a host about to use this resolver with
    /// another IR (the editor's recompile). Since T-263b another IR's ids need no reset; this is
    /// for names that now mean another file (image meta changed, a file forgotten with
    /// forgetFile()); an IR rebuilt over the same memory and opened again is noticed without it
    /// (T-263c: embedded bytes are checked by hash once per IrView::loadSerial()). Decoded files stay cached
    /// by file path, so an asset that names the same file resolves without decoding again --
    /// T-452: and without reading the file again (nor, for a size read from its header only,
    /// reading the header again); revalidate() is what notices a file changed on disk.
    /// T-263f: the decodes go with their slots, because once the slots are forgotten
    /// releaseSource() of an IR finds nothing of that IR's any more. T-263g: with every slot gone
    /// no live source holds any decode, so each one -- of a file or of an embedded image -- moves
    /// to the idle list (see setIdleDecodeBudget()), from where the next lookup of the same file
    /// path / the same embedded bytes takes it back without decoding. What does not fit the
    /// budget, a failed decode, and a decode nothing can find again (its file was forgotten by
    /// forgetFile() or revalidate()) are freed. So calling this before every recompile keeps one
    /// document's worth of pixels plus at most the budget, and an image that did not change is
    /// not decoded again while it fits the budget. Every imageView() / compressedImage() pointer
    /// obtained before this call counts as invalid after it (as after releaseSource()).
    void resetAssetIds();
    /// T-262a: forgets the decoded image cached for the file `file` (relative to the root, e.g.
    /// "atlas/0.png"), so the next asset that reads it decodes it again -- for a file rewritten
    /// under the same name (an atlas page after a re-pack). Assets already resolved keep their
    /// entry (see resetAssetIds()), and every pointer handed out stays valid. T-263g: when the
    /// decode is an idle one (no live source uses it) it is freed at once.
    void forgetFile(std::string_view file);
    /// T-263c: compares every decoded file with the disk -- its size and modification time, as
    /// they were when it was decoded -- and discards the cached decode of each one that was
    /// rewritten, is shadowed by a file in a nearer root, or is gone; returns how many were
    /// discarded. The next asset naming such a file reads and decodes it again (the assets that
    /// had resolved to it are looked up afresh; every pointer handed out stays valid, as with
    /// forgetFile()). T-263g: an idle decode (see setIdleDecodeBudget()) is checked like any
    /// other, and freed at once when its file changed.
    /// Header-only sizes of files not decoded yet are compared the same way (T-452; one read
    /// while no file-status function was set is read again). One file status query per cached
    /// file: meant for a reload action (a viewer's reload key, the editor's check after an edit,
    /// at most once a second), never called by the resolver itself -- no per-frame path asks the
    /// disk about a file it has already resolved. Not covered: embedded assets (T-263c checks
    /// those by hash per IR load), files whose reader has no file status (a subclass reading
    /// through its own opener), a file that did not exist when first asked for (resetAssetIds()).
    /// T-263e: the file status comes from the function given to setFileStatusFn(); while none is
    /// set this returns 0 and does nothing, and a file decoded while none was set is never
    /// compared.
    size_t revalidate();
    /// T-263e: the file-status function revalidate() compares the disk through (see
    /// FileStatusFn); nullptr (the default) = none. Set it before the first asset is resolved: a
    /// file's size and modification time are recorded when it is decoded. `Bundle::open` sets
    /// `xt::fileStatusUtf8` on the resolver it creates.
    void setFileStatusFn(FileStatusFn status);
    /// T-263e: drops the slots of the embedded assets whose bytes lie inside `view`'s blob and
    /// lets go of the decode of each one that no remaining slot (another IR embedding the same
    /// image) resolves to. T-263g: a named (file) asset's slot is held by every live source that
    /// looked the name up -- a source is one IrView::loadSerial(), holding from its first lookup
    /// of the name until this call -- and this call ends `view`'s hold on each of them. A decode
    /// that no live source holds any more is not freed at once: it moves to the idle list (see
    /// setIdleDecodeBudget()), a file's decode with its slot, an embedded image's under its
    /// content key, and a later lookup of the name / of bytes with the same content takes it
    /// back without decoding. So reloading a Document any number of times keeps one document's
    /// worth of slots and pixels plus at most the idle budget. A source that is never released
    /// (an IrView the host opened itself and never passed here) keeps what it looked up until
    /// resetAssetIds(). See AssetResolver::releaseSource for who calls it.
    void releaseSource(const IrView& view) override;
    /// T-263g: how many bytes of idle decodes are kept -- decodes no live source uses any more
    /// (their source was released, or resetAssetIds() forgot every slot), held back so that
    /// closing and reopening a UI, or recompiling a document whose images did not change, does
    /// not decode them again. Bytes are the decoded pixels (plus the blocks of a DDS). When the
    /// idle decodes exceed the budget the least recently released ones are freed until they fit;
    /// a single decode larger than the whole budget is freed when it becomes idle. Default
    /// 32 MiB; 0 = nothing is kept, every decode is freed the moment its last source lets go.
    /// May be called at any time; lowering it frees at once. The resolver is as single-threaded
    /// as before (the host serialises every call).
    void setIdleDecodeBudget(size_t bytes);

private:
    friend struct FileAssetResolverTestAccess;  // T-263e: Tests/unit counts the slots and decoded bytes.

    /// One decoded image (a file, or a whole atlas page), owned by the resolver.
    struct Entry {
        ImageData image;
        /// Set when the file was a DDS: its level-0 blocks for the GPU.
        bool hasCompressed = false;
        CompressedImage compressed;
        bool ok = false;
        /// T-263g: on `m_idle` -- no live source resolves to it.
        bool idle = false;
    };
    /// What one asset resolved to (per variant).
    struct Slot {
        /// Index into `m_entries` + 1; 0 = not attempted; SIZE_MAX = attempted and failed (a 2x
        /// failure means "use the 1x entry").
        size_t entry = 0;
        /// T-160: the header-only size (1x slots only), meaningful when `headerOk`.
        Vec2 headerSize{};
        bool headerOk = false;
    };
    /// A non-embedded asset's slot, found by the asset's name in its IR.
    struct NamedSlot {
        std::string name;
        Slot slot;
        /// T-263g: the IrView::loadSerial() of every live source that looked the name up (each
        /// once), until its releaseSource(). Empty: the slot is only kept for its idle decode,
        /// so that the next source finds it without reading the file.
        std::vector<uint32_t> sources;
        /// T-263g: left without a source while another slot took its decode back from the idle
        /// list; dropped when that decode goes idle again unless a source looked the name up in
        /// between (otherwise names that are never used again would pile up on a shared decode).
        bool lapsed = false;
    };
    /// An embedded asset's slot, found by where its bytes are (inside its IR blob) and confirmed
    /// by their content hash (T-263c: a reloaded blob may reuse the address and size for another
    /// image). `serials` are the IrView::loadSerial()s the hash was checked for, so the bytes
    /// are hashed once per load of an IR, not on every lookup; T-263g ruling 9: several of them,
    /// most recently used first, because two Documents loaded from one host-owned blob are two
    /// views over the same bytes and used to re-hash them on every lookup from the other view.
    /// A view beyond `kVerifiedSerials` replaces the least recently used one; another image at
    /// the address (a different hash) voids them all.
    static constexpr size_t kVerifiedSerials = 4;
    struct EmbeddedSlot {
        const uint8_t* data = nullptr;
        size_t size = 0;
        uint32_t serials[kVerifiedSerials] = {};
        uint8_t serialCount = 0;
        uint64_t hash = 0;
        Slot slot;
    };
    /// The slot of `asset` as `view` names it (creating an empty one on first use); nullptr when
    /// `asset` is outside the ASST table. The pointer is valid until the next slotFor() call.
    Slot* slotFor(AssetId asset, const IrView& view, int variant);
    /// Decodes `asset` (variant 0 = 1x, 1 = its 2x, T-345 2 = its 3x variant) on first use;
    /// returns the entry, or nullptr when the asset cannot be decoded. A variant falls back to the
    /// next smaller one when it does not exist.
    const Entry* ensureDecoded(AssetId asset, const IrView& view, int variant);
    /// Reads the bytes of the file behind `asset` (or its 2x / 3x variant for `variant` 1 / 2;
    /// false when there is none). `fileKey` receives the path the bytes came from (for page
    /// sharing).
    bool readAssetBytes(AssetId asset, const IrView& view, int variant, std::vector<uint8_t>& out,
                        std::string& fileKey);
    /// T-452: the files (below the roots) readAssetBytes() tries for the named asset `asset` of
    /// `view`, in order, appended to `out` (none for an `engine://` asset or a variant with no
    /// file); false for an embedded asset or one outside the ASST table.
    bool fileCandidates(AssetId asset, const IrView& view, int variant, std::vector<std::string>& out) const;
    /// T-452: the `m_entries` index of the decode of the file `key` (a path below the roots, or an
    /// embedded image's content key); kFailed-like SIZE_MAX when none is cached.
    size_t cachedFile(std::string_view key) const;
    /// Decodes `bytes` into a new entry (recording `fileKey` with the file's stamp when non-empty)
    /// and returns its `Slot::entry` value (index + 1).
    size_t decodeNew(const std::vector<uint8_t>& bytes, const std::string& fileKey);
    /// T-452: the header-only size of the first of `files` that exists -- from `m_headerSizes`, or
    /// read from the file and recorded there; false when none exists or its header tells no size.
    bool headerSizeOf(const std::vector<std::string>& files, int& width, int& height);
    /// Decodes `bytes` (any decoder format, or DDS) into `entry`.
    void decodeInto(const std::vector<uint8_t>& bytes, Entry& entry);
    /// T-263e / T-263f / T-263g: lets go of the decode of each `Slot::entry` value in `dropped`
    /// (the entries of slots just removed or left without a source) that no slot a live source
    /// holds resolves to: onto the idle list, or freed (see idleEntry()); then trims the idle
    /// list to the budget. Shared by releaseSource() and resetAssetIds().
    void releaseEntries(const std::vector<size_t>& dropped);
    /// T-263g: whether a slot some live source holds (any embedded slot, a named slot with a
    /// source) resolves to the `Slot::entry` value `resolved`.
    bool entryRetained(size_t resolved) const;
    /// T-263g: `m_entries[index]`, which no live source uses any more, goes to the end of the
    /// idle list -- or is freed at once when the budget is 0, the decode failed, it is larger
    /// than the whole budget, or nothing could find it again (no file / content key and no
    /// source-less named slot).
    void idleEntry(size_t index);
    /// T-263g: takes `m_entries[index]` off the idle list when it is on it (a source uses it
    /// again); nothing is decoded.
    void reviveEntry(size_t index);
    /// T-263e's release: frees the pixels of `m_entries[index]`, removes its file / content key
    /// and (T-263g) the source-less named slots that resolved to it, and puts the index on
    /// `m_freeEntries`. The entry is not on `m_idle` (any more) when this is called.
    void freeEntry(size_t index);
    /// T-263g: frees idle decodes, least recently released first, until they fit the budget.
    void trimIdle();

    std::vector<std::string> m_rootDirs;
    ImageDecodeFn m_decode;
    FileStatusFn m_fileStatus = nullptr;  // T-263e
    // Decoded entries; a deque so growing it never moves existing entries (imageView() hands out
    // their addresses). Atlas pages are shared: `m_fileEntry` maps a file path to its index.
    std::deque<Entry> m_entries;
    // T-263e: indices of `m_entries` whose decode releaseSource() freed, taken again by the next
    // decode before the deque grows.
    std::vector<size_t> m_freeEntries;
    /// One decoded file: its key (the path below the root, or an embedded image's content key),
    /// its index in `m_entries` and, T-263c, the file's size and modification time when it was
    /// read (`hasStamp` false: not a file whose status could be asked).
    struct FileEntry {
        std::string key;
        size_t entry = 0;
        bool hasStamp = false;
        uint64_t size = 0;
        uint64_t mtime = 0;
    };
    std::vector<FileEntry> m_fileEntry;
    /// T-263g: one decode no live source uses, kept in case one does again. `bytes` is what it
    /// counts against the budget: its decoded pixels plus its DDS blocks.
    struct IdleDecode {
        size_t entry = 0;  // index into `m_entries`
        size_t bytes = 0;
    };
    std::vector<IdleDecode> m_idle;  // in release order: the front is the first to go
    size_t m_idleBytes = 0;          // the sum of `m_idle[i].bytes`
    size_t m_idleBudget = size_t{32} * 1024 * 1024;
    /// T-263g ruling 9: how often an embedded asset's bytes were hashed (Tests/unit measures it).
    size_t m_embeddedHashes = 0;
    /// T-452: test hooks, not behaviour -- how often readBytes() was called (a whole-file read)
    /// and how often a file status was asked (fileStamp()).
    size_t m_readBytesCalls = 0;
    mutable size_t m_statCalls = 0;
    /// T-452: the header-only size of a file nobody decoded yet (T-160), kept across
    /// resetAssetIds() by the file's path with its size and modification time when it was read
    /// (`hasStamp` false: no status could be asked); revalidate() compares it like a decode.
    struct HeaderSize {
        std::string key;
        int width = 0;
        int height = 0;
        bool hasStamp = false;
        uint64_t size = 0;
        uint64_t mtime = 0;
    };
    std::vector<HeaderSize> m_headerSizes;
    /// T-263c: the size and modification time of `root/name` in the first root that has it;
    /// false when none does. T-263e: asked through `m_fileStatus`; false when none is set.
    bool fileStamp(std::string_view name, uint64_t& size, uint64_t& mtime) const;
    // T-263b A6: what each asset resolved to, per variant (0 = 1x, 1 = 2x). Never indexed by
    // AssetId, which restarts at 0 in every IR while one resolver serves all the Documents of a
    // Context or bundle: a non-embedded asset is found by its name (sorted by name), an embedded
    // one by the address of its bytes (sorted by address), confirmed by their size and hash.
    std::vector<NamedSlot> m_named[3];
    std::vector<EmbeddedSlot> m_embedded[3];
};

/// T-199 (26 §26.3.4, 22 §22.10): the name scheme of an asset packed into a bundle atlas page:
/// `atlas:<n>#<asset>` (ASST kind 3) names page `<n>` (the file `atlas/<n>.png` or `.dds`) and the
/// original asset name after `#`, whose cell `Bundle::open` registers through
/// AssetResolver::setImageMeta.
inline constexpr std::string_view kAtlasAssetScheme = "atlas:";

/// true when `name` starts with `kAtlasAssetScheme`.
bool isAtlasAssetName(std::string_view name);
/// The `<n>` part of `atlas:<n>#<asset>` (empty when `name` is not an atlas name).
std::string_view atlasAssetPage(std::string_view name);
/// The `<asset>` part of `atlas:<n>#<asset>` (`name` itself when it is not an atlas name).
std::string_view atlasAssetSource(std::string_view name);
/// true when `asset` is a valid ASST entry of `view` whose name is an atlas name.
bool isAtlasAsset(AssetId asset, const IrView& view);

/// The asset-name scheme of an external texture (Docs/tasks/T-150.md): `<img
/// src="engine://scene">` names the host texture bound as "scene" through
/// `AssetResolver::setExternalTexture`. The compiler keeps such a name verbatim (no file lookup,
/// no embedding, no bundle asset); the software renderer draws nothing for it.
inline constexpr std::string_view kExternalAssetScheme = "engine://";

/// true when `name` starts with `kExternalAssetScheme`.
bool isExternalAssetName(std::string_view name);
/// `name` without the scheme; `name` itself when it is not external.
std::string_view externalAssetName(std::string_view name);
/// true when `asset` is a valid ASST entry of `view` whose name is external.
bool isExternalAsset(AssetId asset, const IrView& view);

}  // namespace xt
