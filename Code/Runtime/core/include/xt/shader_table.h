#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "xt/command_list.h"
#include "xt/types.h"

/// Custom effect shaders of a loaded document (Docs/design/18-effects.md §18.5, §18.7, T-203):
/// the IR's `SHDR` chunk unpacked into ShaderRef form once at load. The Painter copies the entries
/// a frame uses into its CommandList; Document::shaderRef() exposes them to hosts (the CryEngine
/// contract: HLSL `source`, parameter table, `expandPx`, `fallback`).

namespace xt {

class IrView;

struct ShaderTable {
    /// One entry per `.xtfx` asset; the offsets index `params` and `text` below.
    std::vector<ShaderRef> refs;
    /// ShaderRef::paramOffset/paramCount pool.
    std::vector<ShaderParamDesc> params;
    /// HLSL sources, GLSL modules and NUL-separated parameter names (ShaderRef's text ranges).
    std::string text;

    /// Replaces the table with `ir`'s SHDR chunk (empty when it has none).
    void build(const IrView& ir);
    /// The entry of `asset`, or nullptr.
    const ShaderRef* find(AssetId asset) const;
    /// Appends `asset`'s entry (params and text included, offsets rebased), stamped with `source`
    /// (T-263d: the painted Document's index in CommandList::sources, 0 when the list names
    /// none), to `out` unless `out` already has one for (`source`, `asset`) -- two Documents'
    /// shader of the same id are two entries. No-op for an unknown asset.
    void copyTo(AssetId asset, uint16_t source, CommandList& out) const;
};

}  // namespace xt
