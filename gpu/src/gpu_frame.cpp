#include "xt/gpu/gpu_frame.h"

#include <cstdio>

namespace xt::gpu {

void GpuFrame::clear() {
    vertices.clear();
    indices.clear();
    cmds.clear();
    passes.clear();
    params.clear();
    transients.clear();
    cacheStores.clear();
    particleInstances.clear();
}

namespace {

const char* shadingName(Shading s) {
    switch (s) {
        case Shading::Solid:
            return "Solid";
        case Shading::Gradient:
            return "Gradient";
        case Shading::Texture:
            return "Texture";
        case Shading::Glyph:
            return "Glyph";
        case Shading::StencilOnly:
            return "StencilOnly";
        case Shading::Composite:
            return "Composite";
        case Shading::Effect:
            return "Effect";
        case Shading::TextureStraight:
            return "TextureStraight";
        case Shading::ParticleTexture:
            return "ParticleTexture";
        case Shading::ParticleSolid:
            return "ParticleSolid";
    }
    return "?";
}

const char* blendName(Blend b) {
    switch (b) {
        case Blend::Replace:
            return "Replace";
        case Blend::SrcOver:
            return "SrcOver";
        case Blend::Additive:
            return "Additive";
    }
    return "?";
}

const char* stencilName(StencilOp op) {
    switch (op) {
        case StencilOp::None:
            return "None";
        case StencilOp::WriteIncrWrap:
            return "WriteIncrWrap";
        case StencilOp::WriteInvert:
            return "WriteInvert";
        case StencilOp::TestNotEqualZero:
            return "TestNotEqualZero";
        case StencilOp::TestEqualRef:
            return "TestEqualRef";
        case StencilOp::ClearRect:
            return "ClearRect";
        case StencilOp::ClipPush:
            return "ClipPush";
        case StencilOp::ClipPop:
            return "ClipPop";
    }
    return "?";
}

const char* programName(EffectProgram p) {
    switch (p) {
        case EffectProgram::None:
            return "None";
        case EffectProgram::GaussScalar:
            return "GaussScalar";
        case EffectProgram::GaussColor:
            return "GaussColor";
        case EffectProgram::Dilate:
            return "Dilate";
        case EffectProgram::AlphaExtract:
            return "AlphaExtract";
        case EffectProgram::Sdf:
            return "Sdf";
        case EffectProgram::ColorMatrix:
            return "ColorMatrix";
        case EffectProgram::GradientMask:
            return "GradientMask";
        case EffectProgram::Shine:
            return "Shine";
        case EffectProgram::ShadowGlow:
            return "ShadowGlow";
        case EffectProgram::Outline:
            return "Outline";
        case EffectProgram::Backdrop:
            return "Backdrop";
        case EffectProgram::MaskApply:
            return "MaskApply";
        case EffectProgram::Custom:
            return "Custom";
        case EffectProgram::Downsample:
            return "Downsample";
        case EffectProgram::Kawase:
            return "Kawase";
        case EffectProgram::LevelMix:
            return "LevelMix";
    }
    return "?";
}

const char* formatName(TextureFormat f) {
    switch (f) {
        case TextureFormat::Rgba8:
            return "rgba8";
        case TextureFormat::R8:
            return "r8";
        case TextureFormat::R32F:
            return "r32f";
        case TextureFormat::Bc1:
            return "bc1";
        case TextureFormat::Bc3:
            return "bc3";
        case TextureFormat::Bc7:
            return "bc7";
    }
    return "?";
}

const char* samplerName(Sampler s) {
    switch (s) {
        case Sampler::PointClamp:
            return "point";
        case Sampler::LinearClamp:
            return "linear";
    }
    return "?";
}

}  // namespace

void dumpGpuFrame(const GpuFrame& frame, std::string& out) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "target %dx%d vertices=%zu indices=%zu cmds=%zu passes=%zu params=%zu\n",
                  frame.targetWidth, frame.targetHeight, frame.vertices.size(), frame.indices.size(),
                  frame.cmds.size(), frame.passes.size(), frame.params.size());
    out += buf;
    for (size_t i = 0; i < frame.transients.size(); ++i) {
        const TransientTargetDesc& t = frame.transients[i];
        std::snprintf(buf, sizeof(buf), "transient %zu: %ux%u %s stencil=%d\n", i + 1, t.width, t.height,
                      formatName(t.format), t.needStencil);
        out += buf;
    }
    for (size_t p = 0; p < frame.passes.size(); ++p) {
        const Pass& pass = frame.passes[p];
        std::snprintf(buf, sizeof(buf),
                      "pass %zu: target=%u clear=%d copy=%d from=%u view=(%d,%d,%d,%d) cmds=%u\n", p,
                      pass.target, pass.clear, pass.copy, pass.copySource, pass.viewX, pass.viewY, pass.viewW,
                      pass.viewH, pass.cmdCount);
        out += buf;
        for (uint32_t c = pass.firstCmd; c < pass.firstCmd + pass.cmdCount && c < frame.cmds.size(); ++c) {
            const DrawCmd& cmd = frame.cmds[c];
            std::snprintf(buf, sizeof(buf), "  %u %s %s %s ref=%u", c, shadingName(cmd.shading),
                          blendName(cmd.blend), stencilName(cmd.stencilOp), cmd.stencilRef);
            out += buf;
            if (cmd.shading == Shading::Effect) {
                std::snprintf(buf, sizeof(buf), " program=%s", programName(cmd.program));
                out += buf;
            }
            if (cmd.kind == DrawKind::Particles) {  // T-202
                std::snprintf(buf, sizeof(buf), " particles=%u+%u", cmd.firstInstance, cmd.instanceCount);
                out += buf;
            }
            std::snprintf(buf, sizeof(buf), " tex=%s src=%u aux=%u %s idx=%u verts=%u clip=(%d,%d,%d,%d)\n",
                          cmd.texture != 0 ? "yes" : "no", cmd.src, cmd.aux, samplerName(cmd.sampler),
                          cmd.indexCount, cmd.vertexCount, cmd.clipX, cmd.clipY, cmd.clipW, cmd.clipH);
            out += buf;
        }
    }
    // T-162: after the passes, so frames without stores dump exactly as before.
    for (size_t i = 0; i < frame.cacheStores.size(); ++i) {
        const CacheStore& s = frame.cacheStores[i];
        std::snprintf(buf, sizeof(buf), "cache store %zu: pass=%u src=%u tex=%s\n", i, s.pass, s.src,
                      s.texture != 0 ? "yes" : "no");
        out += buf;
    }
}

}  // namespace xt::gpu
