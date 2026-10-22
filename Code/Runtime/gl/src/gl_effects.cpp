#include "gl_effects.h"

#include <cstring>

#include "gl_shaders.h"
#include "gl_util.h"
#include "xt/command_list.h"  // customParamSlots() (T-203).

namespace xt::gl {

namespace {

template <class T>
bool readParams(const float* params, size_t available, T& out) {
    if (params == nullptr || available * sizeof(float) < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, params, sizeof(T));
    return true;
}

/// Indexed by EffectPrograms::U.
const char* const kUniformNames[] = {
    "uNdc",     "uTexelSize",      "uDir",       "uTapCount",    "uWeights",      "uOffsets",  "uConstEdge",
    "uOutside", "uRadius",         "uReadAlpha", "uInvert",      "uSearchRadius", "uM",        "uOffset",
    "uOpacity", "uLutWidth",       "uFbSize",    "uInvXf",       "uP0",           "uP1",       "uRadial",
    "uCenter",  "uEdge0",          "uEdge1",     "uColor",       "uIntensity",    "uOffsetUV", "uScale",
    "uColorA",  "uEffectAlphaMul", "uLineOnTop", "uMaskOutside", "uPosition",     "uSoftness", "uMode",
    "uSrcSize", "uDstSize",        "uMix",
};

}  // namespace

bool EffectPrograms::init(std::string& err) {
    using gpu::EffectProgram;
    static_assert(sizeof(kUniformNames) / sizeof(kUniformNames[0]) == kUniformCount);
    struct Entry {
        EffectProgram program;
        const char* frag;
    };
    const Entry entries[] = {
        {EffectProgram::GaussScalar, shaders::kFxGaussScalarFrag},
        {EffectProgram::GaussColor, shaders::kFxGaussColorFrag},
        {EffectProgram::Dilate, shaders::kFxDilateFrag},
        {EffectProgram::AlphaExtract, shaders::kFxAlphaExtractFrag},
        {EffectProgram::Sdf, shaders::kFxSdfFrag},
        {EffectProgram::ColorMatrix, shaders::kFxColorMatrixFrag},
        {EffectProgram::GradientMask, shaders::kFxGradientMaskFrag},
        {EffectProgram::Shine, shaders::kFxShineFrag},
        {EffectProgram::ShadowGlow, shaders::kFxShadowGlowFrag},
        {EffectProgram::Outline, shaders::kFxOutlineFrag},
        {EffectProgram::Backdrop, shaders::kFxBackdropFrag},
        {EffectProgram::MaskApply, shaders::kFxMaskApplyFrag},
        {EffectProgram::Downsample, shaders::kFxDownsampleFrag},  // T-343
        {EffectProgram::Kawase, shaders::kFxKawaseFrag},
        {EffectProgram::LevelMix, shaders::kFxLevelMixFrag},
    };
    GLint previous = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
    for (const Entry& e : entries) {
        GLuint prog = linkProgram(shaders::kQuadVert, e.frag, err);
        if (prog == 0) {
            glUseProgram(static_cast<GLuint>(previous));
            return false;
        }
        Program& p = m_programs[static_cast<int>(e.program)];
        p.id = prog;
        for (int u = 0; u < kUniformCount; ++u) {
            p.locs[u] = glGetUniformLocation(prog, kUniformNames[u]);
        }
        p.shadow = UniformShadow{};
        // Sampler units never change: unit 0 = src (named uTex or uSrc), 1 = aux (uMaskA,
        // uSdf, uBackdrop, uMask), 2 = the LUT (uLut). A -1 location is a no-op.
        glUseProgram(prog);
        glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
        glUniform1i(glGetUniformLocation(prog, "uSrc"), 0);
        glUniform1i(glGetUniformLocation(prog, "uMaskA"), 1);
        glUniform1i(glGetUniformLocation(prog, "uSdf"), 1);
        glUniform1i(glGetUniformLocation(prog, "uBackdrop"), 1);
        glUniform1i(glGetUniformLocation(prog, "uMask"), 1);
        glUniform1i(glGetUniformLocation(prog, "uLut"), 2);
        glUniform1f(p.locs[uLutWidth], static_cast<float>(gpu::kGradientLutWidth));
    }
    glUseProgram(static_cast<GLuint>(previous));
    err.clear();
    return true;
}

void EffectPrograms::shutdown() {
    for (Program& p : m_programs) {
        if (p.id != 0) {
            glDeleteProgram(p.id);
        }
        p = Program{};
    }
}

bool EffectPrograms::bind(gpu::EffectProgram program, const float* params, size_t available, GLuint srcTex,
                          GLuint auxTex, GLuint lutTex, const float* ndc9, GlStateCache& cache) {
    using gpu::EffectProgram;
    const int idx = static_cast<int>(program);
    if (idx <= 0 || idx >= kProgramCount || m_programs[idx].id == 0) {
        return false;
    }
    Program& p = m_programs[idx];
    UniformShadow& u = p.shadow;
    u.stats = &cache.stats;
    const GLint* L = p.locs;
    cache.useProgram(p.id);
    cache.bindTexture(0, srcTex);
    cache.bindTexture(1, auxTex);
    cache.bindTexture(2, lutTex);
    u.setMat3(L[uNdc], ndc9);

    switch (program) {
        case EffectProgram::Downsample: {  // T-343
            gpu::DownsampleParams dp{};
            if (!readParams(params, available, dp)) return false;
            u.set2f(L[uSrcSize], dp.srcW, dp.srcH);
            u.set2f(L[uDstSize], dp.dstW, dp.dstH);
            return true;
        }
        case EffectProgram::Kawase: {  // T-343
            gpu::KawaseParams kp{};
            if (!readParams(params, available, kp)) return false;
            u.set2f(L[uTexelSize], kp.texelW, kp.texelH);
            u.set1f(L[uOffset], kp.offset);
            return true;
        }
        case EffectProgram::LevelMix: {  // T-343
            gpu::LevelMixParams mp{};
            if (!readParams(params, available, mp)) return false;
            u.set2f(L[uScale], mp.scaleU, mp.scaleV);
            u.set2f(L[uOffsetUV], mp.offU, mp.offV);
            u.set1f(L[uMix], mp.t);
            return true;
        }
        case EffectProgram::GaussScalar:
        case EffectProgram::GaussColor: {
            gpu::GaussParams gp{};
            if (!readParams(params, available, gp)) return false;
            u.set2f(L[uTexelSize], gp.texelW, gp.texelH);
            u.set2f(L[uDir], gp.dirX, gp.dirY);
            u.set1i(L[uTapCount], static_cast<int>(gp.tapCount));
            u.set1fv(L[uWeights], 33, gp.weights);
            u.set1fv(L[uOffsets], 33, gp.offsets);
            u.set1i(L[uConstEdge], gp.constEdge != 0.0f ? 1 : 0);
            u.set1f(L[uOutside], gp.outside);
            return true;
        }
        case EffectProgram::Dilate: {
            gpu::DilateParams dp{};
            if (!readParams(params, available, dp)) return false;
            u.set2f(L[uTexelSize], dp.texelW, dp.texelH);
            u.set2f(L[uDir], dp.dirX, dp.dirY);
            u.set1i(L[uRadius], static_cast<int>(dp.radius));
            u.set1i(L[uReadAlpha], dp.readAlpha != 0.0f ? 1 : 0);
            u.set1f(L[uOutside], dp.outside);
            return true;
        }
        case EffectProgram::AlphaExtract: {
            gpu::AlphaExtractParams ap{};
            if (!readParams(params, available, ap)) return false;
            u.set1i(L[uInvert], ap.invert != 0.0f ? 1 : 0);
            return true;
        }
        case EffectProgram::Sdf: {
            gpu::SdfParams sp{};
            if (!readParams(params, available, sp)) return false;
            u.set2f(L[uTexelSize], sp.texelW, sp.texelH);
            u.set1f(L[uSearchRadius], sp.searchRadius);
            return true;
        }
        case EffectProgram::ColorMatrix: {
            gpu::ColorMatrixParams cp{};
            if (!readParams(params, available, cp)) return false;
            // GLSL mat4 is column-major; row i (output channel), column j (input) -> m[j][i].
            float cols[16] = {};
            for (int col = 0; col < 3; ++col) {
                for (int row = 0; row < 3; ++row) {
                    cols[col * 4 + row] = cp.m[row * 3 + col];
                }
            }
            cols[15] = 1.0f;
            u.setMat4(L[uM], cols);
            u.set4f(L[uOffset], cp.offset[0], cp.offset[1], cp.offset[2], 0.0f);
            u.set1f(L[uOpacity], cp.opacity);
            return true;
        }
        case EffectProgram::GradientMask: {
            gpu::GradientMaskParams gm{};
            if (!readParams(params, available, gm)) return false;
            // GLSL mat3 columns (a,b,0), (c,d,0), (e,f,1) reproduce apply(m, p).
            const float inv[9] = {gm.invXf[0], gm.invXf[1], 0.0f,        gm.invXf[2], gm.invXf[3],
                                  0.0f,        gm.invXf[4], gm.invXf[5], 1.0f};
            u.set2f(L[uFbSize], gm.targetW, gm.targetH);
            u.setMat3(L[uInvXf], inv);
            u.set2f(L[uP0], gm.p0x, gm.p0y);
            u.set2f(L[uP1], gm.p1x, gm.p1y);
            u.set1i(L[uRadial], gm.radial != 0.0f ? 1 : 0);
            u.set1i(L[uInvert], gm.invert != 0.0f ? 1 : 0);
            return true;
        }
        case EffectProgram::Shine: {
            gpu::ShineParams sh{};
            if (!readParams(params, available, sh)) return false;
            u.set2f(L[uFbSize], sh.targetW, sh.targetH);
            u.set2f(L[uDir], sh.dirX, sh.dirY);
            u.set1f(L[uCenter], sh.center);
            u.set1f(L[uEdge0], sh.edge0);
            u.set1f(L[uEdge1], sh.edge1);
            u.set3f(L[uColor], sh.r, sh.g, sh.b);
            u.set1f(L[uIntensity], sh.intensity);
            return true;
        }
        case EffectProgram::ShadowGlow: {
            gpu::ShadowGlowParams sg{};
            if (!readParams(params, available, sg)) return false;
            // gpu_frame.h's offset is y-down; kQuadVert's v axis is up, so negate v.
            u.set2f(L[uOffsetUV], sg.offsetU, -sg.offsetV);
            u.set1f(L[uScale], sg.scale);
            u.set3f(L[uColor], sg.r, sg.g, sg.b);
            u.set1f(L[uColorA], sg.a);
            u.set1i(L[uEffectAlphaMul], sg.effectAlphaMul != 0.0f ? 1 : 0);
            u.set1i(L[uLineOnTop], sg.lineOnTop != 0.0f ? 1 : 0);
            u.set1f(L[uMaskOutside], sg.maskOutside);
            return true;
        }
        case EffectProgram::Outline: {
            gpu::OutlineParams op{};
            if (!readParams(params, available, op)) return false;
            u.set1f(L[uRadius], op.radius);
            u.set1f(L[uSoftness], op.softness);
            u.set1i(L[uPosition], static_cast<int>(op.position));
            u.set3f(L[uColor], op.r, op.g, op.b);
            u.set1f(L[uColorA], op.a);
            return true;
        }
        case EffectProgram::Backdrop:
            return true;
        case EffectProgram::MaskApply: {
            gpu::MaskApplyParams mp{};
            if (!readParams(params, available, mp)) return false;
            u.set1i(L[uMode], static_cast<int>(mp.mode));
            return true;
        }
        case EffectProgram::None:
        default:
            return false;
    }
}

// -------------------------------------------------------------------------------------------
// T-203: custom effects.
// -------------------------------------------------------------------------------------------

namespace {

/// Wraps an `.xtfx` GLSL module into a complete fragment shader (Docs/design/18-effects.md §18.7's
/// fixed interface). `xtTexCoord` maps the top-down `XtInput.uv` convention onto the bottom-up FBO
/// texture (kQuadVert's `vUV`); the layer is premultiplied, `xtSample` (in the module)
/// un-premultiplies it, and `main` premultiplies `xtMain`'s straight result.
std::string wrapCustomModule(std::string_view module) {
    std::string src =
        "#version 330 core\n"
        "in vec2 vUV;\n"
        "out vec4 fragColor;\n"
        "vec2 xtTexCoord(vec2 uv) { return vec2(uv.x, 1.0 - uv.y); }\n";
    src.append(module);
    src +=
        "\nvoid main() {\n"
        "    XtInput xtIn;\n"
        "    xtIn.uv = vec2(vUV.x, 1.0 - vUV.y);\n"
        "    xtIn.texelSize = uTexelSize;\n"
        "    xtIn.layerSize = uLayerSize;\n"
        "    vec4 c = clamp(xtMain(xtIn), 0.0, 1.0);\n"
        "    fragColor = vec4(c.rgb * c.a, c.a);\n"
        "}\n";
    return src;
}

}  // namespace

uint32_t CustomEffectPrograms::prepare(std::string_view glsl, const char* names, const uint8_t* types,
                                       uint32_t count, std::string& warning) {
    for (size_t i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].glsl == glsl) {
            return m_entries[i].id != 0 ? static_cast<uint32_t>(i + 1) : 0;
        }
    }
    Entry e;
    e.glsl.assign(glsl);
    e.types.assign(types, types + count);
    e.slots.assign(count, 0);
    std::string err;
    GLuint prog = 0;
    if (!customParamSlots(e.types.data(), count, e.slots.data())) {
        err = "too many parameters";
    } else {
        const std::string frag = wrapCustomModule(glsl);
        GLint previous = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previous);
        prog = linkProgram(shaders::kQuadVert, frag.c_str(), err);
        if (prog != 0) {
            glUseProgram(prog);
            glUniform1i(glGetUniformLocation(prog, "uLayer"), 0);
            e.uNdc = glGetUniformLocation(prog, "uNdc");
            e.uTime = glGetUniformLocation(prog, "xtTime");
            e.uTexelSize = glGetUniformLocation(prog, "uTexelSize");
            e.uLayerSize = glGetUniformLocation(prog, "uLayerSize");
            const char* name = names;
            for (uint32_t i = 0; i < count; ++i) {
                e.paramLocs.push_back(name != nullptr ? glGetUniformLocation(prog, name) : -1);
                if (name != nullptr) {
                    name += std::strlen(name) + 1;
                }
            }
        }
        glUseProgram(static_cast<GLuint>(previous));
    }
    e.id = prog;
    if (prog == 0) {
        warning = "custom effect shader failed to compile; its fallback is applied instead: " + err;
    }
    m_entries.push_back(std::move(e));
    return prog != 0 ? static_cast<uint32_t>(m_entries.size()) : 0;
}

bool CustomEffectPrograms::bind(const float* params, size_t available, GLuint srcTex, const float* ndc9,
                                GlStateCache& cache) {
    gpu::CustomEffectParams cp{};
    if (!readParams(params, available, cp)) {
        return false;
    }
    const size_t idx = static_cast<size_t>(cp.program);
    if (idx == 0 || idx > m_entries.size() || m_entries[idx - 1].id == 0) {
        return false;
    }
    Entry& e = m_entries[idx - 1];
    UniformShadow& u = e.shadow;
    u.stats = &cache.stats;
    cache.useProgram(e.id);
    cache.bindTexture(0, srcTex);
    u.setMat3(e.uNdc, ndc9);
    u.set1f(e.uTime, cp.time);
    u.set2f(e.uLayerSize, cp.layerW, cp.layerH);
    u.set2f(e.uTexelSize, 1.0f / cp.layerW, 1.0f / cp.layerH);
    for (size_t i = 0; i < e.paramLocs.size(); ++i) {
        const uint8_t s = e.slots[i];
        switch (e.types[i]) {
            case 0:
                u.set1f(e.paramLocs[i], cp.params[s]);
                break;
            case 1:
                u.set2f(e.paramLocs[i], cp.params[s], cp.params[s + 1]);
                break;
            default:
                u.set4f(e.paramLocs[i], cp.colors[s * 4 + 0], cp.colors[s * 4 + 1], cp.colors[s * 4 + 2],
                        cp.colors[s * 4 + 3]);
                break;
        }
    }
    return true;
}

void CustomEffectPrograms::shutdown() {
    for (Entry& e : m_entries) {
        if (e.id != 0) {
            glDeleteProgram(e.id);
        }
    }
    m_entries.clear();
}

}  // namespace xt::gl
