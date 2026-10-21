#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "xt/asset_resolver.h"
#include "xt/ir.h"
#include "xt/types.h"

/// NullAssetResolver: the AssetResolver used by the fuzz targets. It never has any bytes to
/// serve, so every call reports failure -- the closest equivalent to Status::kNotFound that
/// AssetResolver's bool-returning interface exposes. See docs/tasks/T-39.md.

namespace xt::fuzz {

class NullAssetResolver : public AssetResolver {
public:
    bool readBytes(std::string_view /*name*/, std::vector<uint8_t>& /*out*/) override { return false; }
    bool decodeImage(AssetId /*asset*/, const IrView& /*view*/, ImageData& /*out*/) override { return false; }
    Vec2 imageSize(AssetId /*asset*/, const IrView& /*view*/) override { return Vec2{0, 0}; }
};

}  // namespace xt::fuzz
