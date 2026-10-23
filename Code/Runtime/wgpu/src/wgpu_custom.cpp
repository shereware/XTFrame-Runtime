#include <webgpu/webgpu.h>
#include <webgpu/wgpu.h>

#include <cstring>
#include <string>

#include "wgpu_internal.h"
#include "xt/command_list.h"  // customParamSlots() (T-203)

/// Custom effects v1 (T-438 item 13, Docs/design/10-backends.md §10.10.2 `prepareCustomEffect` row,
/// 18 §18.7): the `.xtfx` GLSL 330 module goes through wgpu-native's GLSL frontend (Naga). The
/// module's own `uniform` declarations (xtfx.h: uLayer, xtTime, uTexelSize, uLayerSize and the
/// parameters) are replaced by what Vulkan-style GLSL can bind: one std140 block at set 1 (this
/// device's group 1, `packCustom` fills it) whose values `main` copies into private globals of the
/// original names, and a separate texture + sampler at set 2 bindings 0 and 3 behind a `uLayer`
/// macro. Otherwise the wrapper is gl_effects.cpp's `wrapCustomModule` one to one, without its y
/// flip (WebGPU targets are top-down): `xtTexCoord(uv) = uv`, `xtIn.uv = vUV`, premultiplied output.

namespace xt::wgpu::detail {

namespace {

/// Block layout (floats): [0] xtTime, [4..5] uTexelSize, [6..7] uLayerSize, then one vec4 per
/// parameter from [8].
constexpr uint32_t kParamBase = 8;
constexpr uint32_t kMaxParams = 10;  // 8 float slots + 2 colors at most (customParamSlots)

std::string wrapModule(std::string_view module, const char* names, const uint8_t* types, uint32_t count) {
    std::string src =
        "#version 450\n"
        "layout(location = 0) in vec2 vUV;\n"
        "layout(location = 0) out vec4 fragColor;\n"
        "layout(set = 1, binding = 0, std140) uniform XtCustomBlock { vec4 frame; vec4 sizes; vec4 p[10]; } "
        "xtU;\n"
        "layout(set = 2, binding = 0) uniform texture2D xtLayerTex;\n"
        "layout(set = 2, binding = 3) uniform sampler xtLayerSmp;\n"
        "#define uLayer sampler2D(xtLayerTex, xtLayerSmp)\n"
        "float xtTime;\n"
        "vec2 uTexelSize;\n"
        "vec2 uLayerSize;\n";
    std::string assign =
        "    xtTime = xtU.frame.x;\n"
        "    uTexelSize = xtU.sizes.xy;\n"
        "    uLayerSize = xtU.sizes.zw;\n";
    const char* name = names;
    for (uint32_t i = 0; i < count && name != nullptr; ++i) {
        const char* type = types[i] == 0 ? "float" : (types[i] == 1 ? "vec2" : "vec4");
        const char* swizzle = types[i] == 0 ? ".x" : (types[i] == 1 ? ".xy" : "");
        src += std::string(type) + " " + name + ";\n";
        assign += std::string("    ") + name + " = xtU.p[" + std::to_string(i) + "]" + swizzle + ";\n";
        name += std::strlen(name) + 1;
    }
    src += "vec2 xtTexCoord(vec2 uv) { return uv; }\n";
    // The module without its top-level `uniform` lines (the emitter writes one per line).
    size_t pos = 0;
    while (pos < module.size()) {
        size_t end = module.find('\n', pos);
        if (end == std::string_view::npos) end = module.size();
        const std::string_view line = module.substr(pos, end - pos);
        if (line.substr(0, 8) != "uniform ") {
            src.append(line);
            src += '\n';
        }
        pos = end + 1;
    }
    src += "\nvoid main() {\n";
    src += assign;
    src +=
        "    XtInput xtIn;\n"
        "    xtIn.uv = vUV;\n"
        "    xtIn.texelSize = uTexelSize;\n"
        "    xtIn.layerSize = uLayerSize;\n"
        "    vec4 c = clamp(xtMain(xtIn), 0.0, 1.0);\n"
        "    fragColor = vec4(c.rgb * c.a, c.a);\n"
        "}\n";
    return src;
}

/// gl_shaders.h kQuadVert in Vulkan-style GLSL (set 0 = this device's pass constants), without its v
/// flip: wgpu validates the stage interface by interpolation, and a WGSL output (perspective,
/// center) does not match the sampling-less input Naga makes of a GLSL input.
const char* const kCustomVertex =
    "#version 450\n"
    "layout(location = 0) in vec3 aPosW;\n"
    "layout(location = 1) in vec2 aUV;\n"
    "layout(location = 2) in vec4 aColor;\n"
    "layout(set = 0, binding = 0, std140) uniform XtPass { vec4 m0; vec4 m1; vec4 m2; } xtPass;\n"
    "layout(location = 0) out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 xy = aPosW.xy / aPosW.z;\n"
    "    vec2 p = xtPass.m0.xy * xy.x + xtPass.m1.xy * xy.y + xtPass.m2.xy;\n"
    "    gl_Position = vec4(p * aPosW.z, 0.0, aPosW.z);\n"
    "    vUV = aUV;\n"
    "}\n";

WGPUShaderModule glslModule(WGPUDevice device, const std::string& code, WGPUShaderStage stage) {
    WGPUShaderSourceGLSL src = {};
    src.chain.sType = static_cast<WGPUSType>(WGPUSType_ShaderSourceGLSL);
    src.stage = stage;
    src.code = WGPUStringView{code.data(), code.size()};
    WGPUShaderModuleDescriptor d = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    d.nextInChain = &src.chain;
    d.label = label("xt_wgpu custom effect");
    return wgpuDeviceCreateShaderModule(device, &d);
}

}  // namespace

uint32_t Pipelines::prepareCustom(std::string_view glsl, const char* names, const uint8_t* types,
                                  uint32_t count, std::string& warning) {
    for (size_t i = 0; i < m_custom.size(); ++i) {
        if (m_custom[i].glsl == glsl) {
            return m_custom[i].module != nullptr ? static_cast<uint32_t>(i + 1) : 0;
        }
    }
    Custom c;
    c.glsl.assign(glsl);
    c.types.assign(types, types + count);
    c.slots.assign(count, 0);
    std::string err;
    if (count > kMaxParams || !customParamSlots(c.types.data(), count, c.slots.data())) {
        err = "too many parameters";
    } else {
        const std::string code = wrapModule(glsl, names, types, count);
        pushCreationScopes(m_device);
        if (m_customVertex == nullptr) {
            m_customVertex = glslModule(m_device, kCustomVertex, WGPUShaderStage_Vertex);
        }
        c.module = glslModule(m_device, code, WGPUShaderStage_Fragment);
        err = popCreationScopes(m_device);
        if (err.empty() && c.module != nullptr) {
            // Interface check: one pipeline against this device's layouts (RGBA8, 1x, no stencil).
            m_custom.push_back(c);
            PipelineKey key;
            key.shader = static_cast<uint16_t>(kShaderCount + m_custom.size() - 1);
            pushCreationScopes(m_device);
            WGPURenderPipeline probe = create(key, c.module);
            err = popCreationScopes(m_device);
            if (probe != nullptr) {
                if (err.empty()) {
                    m_pipelines.emplace(key.packed(), probe);
                } else {
                    wgpuRenderPipelineRelease(probe);
                }
            }
            m_custom.pop_back();
        }
        if (!err.empty() && c.module != nullptr) {
            wgpuShaderModuleRelease(c.module);
            c.module = nullptr;
        } else if (c.module == nullptr && err.empty()) {
            err = "shader module creation failed";
        }
    }
    const bool ok = c.module != nullptr;
    if (!ok) {
        warning = "custom effect shader failed to compile; its fallback is applied instead: " + err;
    }
    m_custom.push_back(std::move(c));
    return ok ? static_cast<uint32_t>(m_custom.size()) : 0;
}

bool Pipelines::packCustom(const float* params, size_t available, float* out, uint16_t& shader) const {
    gpu::CustomEffectParams cp{};
    if (params == nullptr || available * sizeof(float) < sizeof(cp)) {
        return false;
    }
    std::memcpy(&cp, params, sizeof(cp));
    const size_t idx = static_cast<size_t>(cp.program);
    if (idx == 0 || idx > m_custom.size() || m_custom[idx - 1].module == nullptr) {
        return false;
    }
    const Custom& c = m_custom[idx - 1];
    shader = static_cast<uint16_t>(kShaderCount + idx - 1);
    out[0] = cp.time;
    out[4] = cp.layerW > 0 ? 1.0f / cp.layerW : 0.0f;
    out[5] = cp.layerH > 0 ? 1.0f / cp.layerH : 0.0f;
    out[6] = cp.layerW;
    out[7] = cp.layerH;
    for (size_t i = 0; i < c.types.size(); ++i) {
        float* p = out + kParamBase + 4 * i;
        const uint8_t s = c.slots[i];
        switch (c.types[i]) {
            case 0:
                p[0] = cp.params[s];
                break;
            case 1:
                p[0] = cp.params[s];
                p[1] = cp.params[s + 1];
                break;
            default:
                std::memcpy(p, &cp.colors[s * 4], 4 * sizeof(float));
                break;
        }
    }
    return true;
}

}  // namespace xt::wgpu::detail
