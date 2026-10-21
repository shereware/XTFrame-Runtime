#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../file_bytes.h"
#include "base64.h"
#include "lottie_model.h"
#include "xt/compile/utf8_path.h"

/// Image assets -> ASST entries (T-16). See docs/design/11-lottie-mapping.md §11.6,
/// docs/tasks/T-16.md "圖片資產". `<fstream>`/`<filesystem>` are allowed in xt_compile.

namespace xt::compile {

namespace {

using detail::readFileBytes;

/// Splits a `data:<mime>;base64,<data>` URI's payload out; returns false if there is no comma
/// (malformed).
bool splitDataUri(const std::string& uri, std::string& base64Part) {
    size_t comma = uri.find(',');
    if (comma == std::string::npos) {
        return false;
    }
    base64Part = uri.substr(comma + 1);
    return true;
}

/// T-261a (K3): true when `name` (an image asset's `u + p`) is a relative path that stays below
/// `rootDir` -- not empty, no drive or root, no `..` segment. The same check as import/'s
/// resolveInside(), which xt_compile cannot include (R does not depend on E).
bool staysBelowRoot(const std::string& rootDir, const std::string& name) {
    namespace fs = std::filesystem;
    if (name.empty()) {
        return false;
    }
    const fs::path relative = pathFromUtf8(name);
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    for (const fs::path& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    if (rootDir.empty()) {
        return true;  // Relative, without `..`: it cannot climb above the working directory.
    }
    fs::path base = pathFromUtf8(rootDir).lexically_normal();
    if (!base.has_filename() && base.has_relative_path()) {
        base = base.parent_path();
    }
    const fs::path inside = (pathFromUtf8(rootDir) / relative).lexically_normal().lexically_relative(base);
    return !inside.empty() && inside != "." && *inside.begin() != "..";
}

}  // namespace

AssetId resolveImageAsset(BuildCtx& ctx, const std::string& assetId, const LottieLayer& layer) {
    auto cached = ctx.imageAssetCache.find(assetId);
    if (cached != ctx.imageAssetCache.end()) {
        return cached->second;
    }

    auto it = ctx.doc.assets.find(assetId);
    if (it == ctx.doc.assets.end() || !it->second.isImage) {
        ctx.sink.error(ctx.file, lottieLayerDesc(layer) + ": image asset \"" + assetId + "\" not found");
        return kInvalidAsset;
    }
    const LottieAsset& asset = it->second;

    AssetBuilder ab;
    ab.kind = 0;  // Image.

    bool isDataUri = asset.p.size() >= 5 && asset.p.compare(0, 5, "data:") == 0;
    if (asset.embed || isDataUri) {
        ab.name = ctx.writer.intern(asset.id);
        std::string base64Part;
        std::vector<uint8_t> bytes;
        if (!splitDataUri(asset.p, base64Part) || !base64Decode(base64Part, bytes)) {
            ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": image asset \"" + asset.id +
                                        "\": malformed data URI; embedding empty bytes");
            bytes.clear();
        }
        ab.embedded = std::move(bytes);
    } else if (std::string name = asset.u + asset.p; !staysBelowRoot(ctx.options.rootDir, name)) {
        // Never read or named: the entry carries only the asset id, as a malformed data URI's does,
        // so the IR stays valid and nothing outside the root is referenced.
        ab.name = ctx.writer.intern(asset.id);
        ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": image asset \"" + asset.id + "\": path \"" +
                                    name + "\" is outside the root directory; skipped");
    } else {
        ab.name = ctx.writer.intern(name);

        bool embedded = false;
        if (ctx.options.embedImages) {
            std::filesystem::path fullPath = ctx.options.rootDir.empty()
                                                 ? pathFromUtf8(name)
                                                 : pathFromUtf8(ctx.options.rootDir) / pathFromUtf8(name);
            std::vector<uint8_t> bytes;
            if (readFileBytes(fullPath, bytes)) {
                ab.embedded = std::move(bytes);
                embedded = true;
            } else {
                ctx.sink.warn(ctx.file, lottieLayerDesc(layer) + ": failed to embed image asset \"" + name +
                                            "\"; kept external");
            }
        }
        if (!embedded && !ctx.options.rootDir.empty()) {
            std::filesystem::path fullPath = pathFromUtf8(ctx.options.rootDir) / pathFromUtf8(name);
            if (!std::filesystem::exists(fullPath)) {
                ctx.sink.warn(ctx.file, "image asset \"" + name + "\" not found");
            }
        }
    }

    AssetId newId = ctx.writer.addAsset(std::move(ab));
    ctx.imageAssetCache.emplace(assetId, newId);
    return newId;
}

}  // namespace xt::compile
