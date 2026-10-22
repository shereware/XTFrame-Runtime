#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "gl_loader.h"
#include "gl_state_cache.h"
#include "xt/gpu/gpu_frame.h"

/// GL programs behind `xt::gpu::EffectProgram` (T-147, Docs/design/10-backends.md §10.5): one
/// GLSL 330 fragment shader per program (gl_shaders.h's T-54 section plus MaskApply), all linked
/// against `kQuadVert`. Pass scheduling (which passes an effect needs, their parameters,
/// scratch targets) lives in xt_gpu (Code/Runtime/gpu/src/gpu_effects.cpp); this class only binds a program
/// and uploads the command's parameter block as uniforms.
///
/// T-159: uniform locations are looked up once at init, sampler-unit uniforms are set once, and
/// every other uniform goes through a per-program `UniformShadow` so a repeated parameter block
/// (the second Gauss pass of a pair, the same blur every frame) costs no glUniform* calls.

namespace xt::gl {

class EffectPrograms {
public:
    /// Compiles every effect program. Returns false on failure (`err` explains why).
    bool init(std::string& err);
    void shutdown();

    /// Binds the program for `program` (through `cache`), sets its uniforms from `params`
    /// (`available` floats at that address, per gpu_frame.h's `*Params` structs) and binds
    /// `srcTex`/`auxTex`/`lutTex` to texture units 0/1/2, which the samplers point at. `ndc9` is
    /// the pass's column-major surface-px -> NDC matrix (`uNdc`, T-151). The caller then draws
    /// the command's window quad. Returns false for an unknown program.
    bool bind(gpu::EffectProgram program, const float* params, size_t available, GLuint srcTex, GLuint auxTex,
              GLuint lutTex, const float* ndc9, GlStateCache& cache);

private:
    static constexpr int kProgramCount = 17;  // T-343: + Downsample, Kawase, LevelMix

    /// Every uniform name any effect shader declares; a program's `locs` holds -1 for the ones
    /// it does not have.
    enum U {
        uNdc,
        uTexelSize,
        uDir,
        uTapCount,
        uWeights,
        uOffsets,
        uConstEdge,
        uOutside,
        uRadius,
        uReadAlpha,
        uInvert,
        uSearchRadius,
        uM,
        uOffset,
        uOpacity,
        uLutWidth,
        uFbSize,
        uInvXf,
        uP0,
        uP1,
        uRadial,
        uCenter,
        uEdge0,
        uEdge1,
        uColor,
        uIntensity,
        uOffsetUV,
        uScale,
        uColorA,
        uEffectAlphaMul,
        uLineOnTop,
        uMaskOutside,
        uPosition,
        uSoftness,
        uMode,
        uSrcSize,  // T-343
        uDstSize,
        uMix,
        kUniformCount
    };
    struct Program {
        GLuint id = 0;
        GLint locs[kUniformCount] = {};
        UniformShadow shadow;
    };
    Program m_programs[kProgramCount];
};

/// T-203 (Docs/design/18-effects.md §18.7): `EffectProgram::Custom`. Each distinct `.xtfx` GLSL
/// module is wrapped (kQuadVert, `#version 330 core`, the y-flip `xtTexCoord`, a `main` that
/// builds `XtInput`, calls `xtMain` and premultiplies its straight-alpha result) and linked once;
/// the cache key is the module text (a CommandList carries no asset names, and equal text is the
/// same program). A module that fails to compile stays cached as failed, so its warning is
/// reported once and every later frame applies the fallback without retrying.
class CustomEffectPrograms {
public:
    /// See gpu::GpuDevice::prepareCustomEffect. `warning` receives the one-time compile failure
    /// message (left untouched otherwise).
    uint32_t prepare(std::string_view glsl, const char* names, const uint8_t* types, uint32_t count,
                     std::string& warning);
    /// Binds program `params.program` (a `gpu::CustomEffectParams` block at `params`), its
    /// uniforms and `srcTex` on unit 0. False for an unknown or failed program.
    bool bind(const float* params, size_t available, GLuint srcTex, const float* ndc9, GlStateCache& cache);
    void shutdown();
    /// Number of cached modules (tests).
    size_t size() const { return m_entries.size(); }

private:
    struct Entry {
        std::string glsl;
        GLuint id = 0;  ///< 0 = the compile failed.
        GLint uNdc = -1, uTime = -1, uTexelSize = -1, uLayerSize = -1;
        std::vector<GLint> paramLocs;
        std::vector<uint8_t> types;
        std::vector<uint8_t> slots;
        UniformShadow shadow;
    };
    std::vector<Entry> m_entries;  ///< program id = index + 1
};

}  // namespace xt::gl
