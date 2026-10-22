#include "xt/shader_table.h"

#include <cstring>

#include "xt/ir.h"

/// See xt/shader_table.h (Docs/design/18-effects.md §18.5, §18.7, T-203).

namespace xt {

void ShaderTable::build(const IrView& ir) {
    refs.clear();
    params.clear();
    text.clear();
    const ShaderChunk& chunk = ir.shaders();
    if (!chunk.present()) {
        return;
    }
    const std::string_view pool = chunk.text();
    for (const IrShaderRef& src : chunk.shaders()) {
        ShaderRef ref{};
        ref.asset = src.asset;
        ref.sourceOffset = static_cast<uint32_t>(text.size());
        ref.sourceLength = src.sourceLength;
        text.append(pool.substr(src.sourceOffset, src.sourceLength));
        ref.glslOffset = static_cast<uint32_t>(text.size());
        ref.glslLength = src.glslLength;
        text.append(pool.substr(src.glslOffset, src.glslLength));
        ref.paramOffset = static_cast<uint32_t>(params.size());
        ref.paramCount = src.paramCount;
        ref.namesOffset = static_cast<uint32_t>(text.size());
        for (uint32_t i = 0; i < src.paramCount; ++i) {
            const IrShaderParam& p = chunk.params()[src.paramOffset + i];
            ShaderParamDesc d{};
            d.name = p.name;
            d.type = p.type;
            std::memcpy(d.defaultValue, p.defaultValue, sizeof(d.defaultValue));
            d.minValue = p.minValue;
            d.maxValue = p.maxValue;
            params.push_back(d);
            text.append(ir.string(p.name));
            text.push_back('\0');
        }
        ref.namesLength = static_cast<uint32_t>(text.size()) - ref.namesOffset;
        ref.expandPx = src.expandPx;
        static_assert(sizeof(EffectInstance) == sizeof(IrEffect));
        std::memcpy(static_cast<void*>(&ref.fallback), &src.fallback, sizeof(EffectInstance));
        refs.push_back(ref);
    }
}

const ShaderRef* ShaderTable::find(AssetId asset) const {
    for (const ShaderRef& r : refs) {
        if (r.asset == asset) {
            return &r;
        }
    }
    return nullptr;
}

void ShaderTable::copyTo(AssetId asset, uint16_t source, CommandList& out) const {
    for (const ShaderRef& existing : out.shaders) {
        if (existing.asset == asset && existing.source == source) {
            return;
        }
    }
    const ShaderRef* src = find(asset);
    if (src == nullptr) {
        return;
    }
    ShaderRef ref = *src;
    ref.source = source;
    const uint32_t textBase = static_cast<uint32_t>(out.shaderText.size());
    const uint32_t first = src->sourceOffset;
    const uint32_t end = src->namesOffset + src->namesLength;  // source, glsl, names are contiguous.
    out.shaderText.insert(out.shaderText.end(), text.begin() + first, text.begin() + end);
    ref.sourceOffset = textBase + (src->sourceOffset - first);
    ref.glslOffset = textBase + (src->glslOffset - first);
    ref.namesOffset = textBase + (src->namesOffset - first);
    ref.paramOffset = static_cast<uint32_t>(out.shaderParams.size());
    out.shaderParams.insert(out.shaderParams.end(), params.begin() + src->paramOffset,
                            params.begin() + src->paramOffset + src->paramCount);
    out.shaders.push_back(ref);
}

}  // namespace xt
