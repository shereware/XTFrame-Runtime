#pragma once

/// GLSL 330 core source strings for xt_gl as a `xt::gpu::GpuDevice` (T-147, docs/design/
/// 10-backends.md §10.5). The frame's draw commands share one vertex shader (`kMainVert`, the
/// `xt::gpu::Vertex` layout) and pick a fragment shader by `Shading`: Solid, Gradient, Texture,
/// Glyph (StencilOnly reuses Solid with color writes off). Composite/Effect passes draw the
/// command's own window quad (T-151: positioned in surface pixels, u,v 0..1 over the source
/// window) with `kQuadVert` and sample the transient targets directly.
///
/// Conventions (gpu_frame.h): every fragment is premultiplied; `Blend::SrcOver` is
/// (ONE, ONE_MINUS_SRC_ALPHA). Uploaded textures are top-down, so their `u,v` are used as-is;
/// FBO textures are bottom-up, so `kQuadVert` flips the builder's y-down `v` (`vUV.y = 1 - v`,
/// v = 0 at the bottom) to address them directly, and per-pixel window positions are
/// reconstructed with a `1 - v` flip where a shader needs them (GradientMask, Shine).

namespace xt::gl::shaders {

/// `aPosW = (x*w, y*w, w)` in device pixels (docs/design/19-3d-space.md §19.7): divided to NDC
/// through `uNdc`, then re-multiplied so the GPU's own perspective divide interpolates `vUV` and
/// `vColor` perspective-correctly. `w == 1` for unprojected commands.
inline constexpr const char* kMainVert = R"(#version 330 core
layout(location = 0) in vec3 aPosW;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat3 uNdc;
out vec2 vUV;
out vec4 vColor;
void main() {
    vec2 xy = aPosW.xy / aPosW.z;
    vec3 p = uNdc * vec3(xy, 1.0);
    gl_Position = vec4(p.xy * aPosW.z, 0.0, aPosW.z);
    vUV = aUV;
    vColor = aColor;
}
)";

inline constexpr const char* kSolidFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

/// `Shading::Gradient` (gpu_frame.h's GradientParams): `vUV` is the path-local position; the
/// stop ramp is a 256x1 straight-color lookup texture; the vertex alpha is the edge-fringe
/// coverage (T-155).
inline constexpr const char* kGradientFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D uLut;
uniform int uKind;        // 2 linear, 3 radial, 4 conic (xt::PaintKind)
uniform vec2 uP0;
uniform vec2 uP1;
uniform vec2 uRadialAxes;
uniform float uPaintAlpha;
uniform float uLutWidth;
void main() {
    vec2 d = uP1 - uP0;
    float lenSq = dot(d, d);
    float t;
    if (lenSq <= 0.0) {
        t = 1.0;
    } else {
        vec2 v = vUV - uP0;
        if (uKind == 4) {
            // T-201: conic -- the clockwise angle from d to v (y down) over a full turn.
            t = atan(d.x * v.y - d.y * v.x, dot(d, v)) * 0.15915494309;
            t = t < 0.0 ? t + 1.0 : t;
        } else {
            t = (uKind == 3) ? length(v / uRadialAxes) : (dot(v, d) / lenSq);
        }
        t = clamp(t, 0.0, 1.0);
    }
    float u = (t * (uLutWidth - 1.0) + 0.5) / uLutWidth;
    vec4 c = texture(uLut, vec2(u, 0.5));
    float a = c.a * uPaintAlpha * vColor.a;
    fragColor = vec4(c.rgb * a, a);
}
)";

inline constexpr const char* kTextureFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    fragColor = texture(uTex, vUV) * vColor;
}
)";

/// T-202 `DrawKind::Particles` (gpu_frame.h `ParticleInstance`): one instance per particle,
/// six vertices each (two triangles, corners from `gl_VertexID`), no vertex buffer of its own.
/// `uXf` maps particle space to surface pixels (column-major mat3 of the command's
/// `ParticleParams::xf`), `uNdc` surface pixels to NDC as in kMainVert.
inline constexpr const char* kParticleVert = R"(#version 330 core
layout(location = 0) in vec4 aCenterSize;
layout(location = 1) in float aRot;
layout(location = 2) in vec4 aColor;
layout(location = 3) in vec4 aUv;
uniform mat3 uNdc;
uniform mat3 uXf;
out vec2 vUV;
out vec4 vColor;
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                 vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
void main() {
    vec2 corner = kCorners[gl_VertexID];
    vec2 d = (corner - 0.5) * aCenterSize.zw;
    float c = cos(aRot);
    float s = sin(aRot);
    vec2 local = aCenterSize.xy + vec2(c * d.x - s * d.y, s * d.x + c * d.y);
    vec3 px = uXf * vec3(local, 1.0);
    vec3 p = uNdc * vec3(px.xy, 1.0);
    gl_Position = vec4(p.xy, 0.0, 1.0);
    vUV = mix(aUv.xy, aUv.zw, corner);
    vColor = aColor;
}
)";

/// T-202 `Shading::ParticleTexture`: texel (premultiplied first when `uStraight` != 0) times the
/// instance color.
inline constexpr const char* kParticleTextureFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uStraight;
void main() {
    vec4 s = texture(uTex, vUV);
    if (uStraight != 0.0) {
        s = vec4(s.rgb * s.a, s.a);
    }
    fragColor = s * vColor;
}
)";

/// T-202 `Shading::ParticleSolid`: the instance color.
inline constexpr const char* kParticleSolidFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
void main() {
    fragColor = vColor;
}
)";

/// `Shading::TextureStraight` (gpu_frame.h, T-150): the texture holds straight alpha (a host
/// texture bound with straightAlpha = true); premultiply the sample before the vertex color.
inline constexpr const char* kTextureStraightFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D uTex;
void main() {
    vec4 s = texture(uTex, vUV);
    fragColor = vec4(s.rgb * s.a, s.a) * vColor;
}
)";

/// `Shading::Glyph`: R8 coverage raised to the run's gamma (docs/design/23-host-integration.md
/// §23.4) times the premultiplied text color.
/// T-178: `uEffect` 1 (Dilate) / 2 (Blur) is a glyph-level text effect of device radius `uRadius`
/// over the glyph's own atlas rectangle `uRect` (texels, [x0, y0, x1, y1)), per
/// xt::GlyphEffect's definitions; texels are read exactly (texelFetch), so the result matches the
/// SW backend's per-glyph bitmap computation.
inline constexpr const char* kGlyphFrag = R"(#version 330 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
uniform sampler2D uTex;
uniform float uGamma;
uniform int uEffect;
uniform float uRadius;
uniform vec4 uRect;
uniform float uTexSize;
float cov(ivec2 t) {
    if (t.x < int(uRect.x) || t.y < int(uRect.y) || t.x >= int(uRect.z) || t.y >= int(uRect.w)) {
        return 0.0;
    }
    return texelFetch(uTex, t, 0).r;
}
void main() {
    if (uEffect == 0) {
        float coverage = pow(texture(uTex, vUV).r, uGamma);
        fragColor = vColor * coverage;
        return;
    }
    ivec2 p = ivec2(floor(vUV * uTexSize));
    float m = 0.0;
    if (uEffect == 1) {
        int k = uRadius > 0.0 ? int(ceil(uRadius)) : 0;
        for (int dy = -k; dy <= k; ++dy) {
            for (int dx = -k; dx <= k; ++dx) {
                float wd = clamp(uRadius + 1.0 - sqrt(float(dx * dx + dy * dy)), 0.0, 1.0);
                if (wd > 0.0) {
                    m = max(m, cov(p + ivec2(dx, dy)) * wd);
                }
            }
        }
    } else {
        float sigma = uRadius * 0.5;
        if (sigma <= 0.0) {
            m = cov(p);
        } else {
            int k = int(ceil(3.0 * sigma));
            float sum = 0.0;
            float acc = 0.0;
            for (int dy = -k; dy <= k; ++dy) {
                float wy = exp(-float(dy * dy) / (2.0 * sigma * sigma));
                for (int dx = -k; dx <= k; ++dx) {
                    float w = wy * exp(-float(dx * dx) / (2.0 * sigma * sigma));
                    acc += w * cov(p + ivec2(dx, dy));
                    sum += w;
                }
            }
            m = acc / sum;
        }
    }
    fragColor = vColor * min(m, 1.0);
}
)";

/// Composite/Effect vertex shader (T-151): the frame's vertex layout through `uNdc` like
/// `kMainVert`, with the builder's y-down `v` flipped so `vUV` addresses a bottom-up FBO texture
/// (v = 0 at the bottom, as the effect shaders below assume).
inline constexpr const char* kQuadVert = R"(#version 330 core
layout(location = 0) in vec3 aPosW;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;
uniform mat3 uNdc;
out vec2 vUV;
void main() {
    vec2 xy = aPosW.xy / aPosW.z;
    vec3 p = uNdc * vec3(xy, 1.0);
    gl_Position = vec4(p.xy * aPosW.z, 0.0, aPosW.z);
    vUV = vec2(aUV.x, 1.0 - aUV.y);
}
)";

/// `Shading::Composite` (gpu_frame.h): `uSrc` is the layer (premultiplied). `uMode == 0`
/// (Normal): output `src * opacity`, blended by the hardware (SrcOver). Otherwise `uDst` holds
/// the backdrop copy and the W3C compositing result is computed here (blending off):
/// `Co = (1 - ab) * as * Cs + ab * as * B(Cb, Cs) + (1 - as) * Cb_premult`.
inline constexpr const char* kCompositeFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uDst;
uniform int uMode;  // 0-16: BlendMode (docs/design/09-command-list.md)
uniform float uOpacity;

float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }

vec3 clipColor(vec3 c) {
    float l = lum(c);
    float n = min(min(c.r, c.g), c.b);
    float x = max(max(c.r, c.g), c.b);
    if (n < 0.0) {
        c = l + (c - l) * (l / max(l - n, 1e-5));
    }
    if (x > 1.0) {
        c = l + (c - l) * ((1.0 - l) / max(x - l, 1e-5));
    }
    return c;
}

vec3 setLum(vec3 c, float l) { return clipColor(c + (l - lum(c))); }

float satOf(vec3 c) { return max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b); }

vec3 setSat(vec3 c, float s) {
    float cmax = max(max(c.r, c.g), c.b);
    float cmin = min(min(c.r, c.g), c.b);
    vec3 result = vec3(0.0);
    if (cmax > cmin) {
        result = (c - cmin) * s / (cmax - cmin);
    }
    return result;
}

vec3 blendChannel(vec3 cb, vec3 cs, int mode) {
    if (mode == 0) return cs;                                    // Normal
    if (mode == 1) return cb * cs;                                // Multiply
    if (mode == 2) return cb + cs - cb * cs;                      // Screen
    if (mode == 3) {                                              // Overlay
        return vec3(cb.r <= 0.5 ? 2.0 * cb.r * cs.r : 1.0 - 2.0 * (1.0 - cb.r) * (1.0 - cs.r),
                    cb.g <= 0.5 ? 2.0 * cb.g * cs.g : 1.0 - 2.0 * (1.0 - cb.g) * (1.0 - cs.g),
                    cb.b <= 0.5 ? 2.0 * cb.b * cs.b : 1.0 - 2.0 * (1.0 - cb.b) * (1.0 - cs.b));
    }
    if (mode == 4) return min(cb, cs);                            // Darken
    if (mode == 5) return max(cb, cs);                            // Lighten
    if (mode == 6) {                                              // ColorDodge
        return vec3(cb.r <= 0.0 ? 0.0 : (cs.r >= 1.0 ? 1.0 : min(1.0, cb.r / (1.0 - cs.r))),
                    cb.g <= 0.0 ? 0.0 : (cs.g >= 1.0 ? 1.0 : min(1.0, cb.g / (1.0 - cs.g))),
                    cb.b <= 0.0 ? 0.0 : (cs.b >= 1.0 ? 1.0 : min(1.0, cb.b / (1.0 - cs.b))));
    }
    if (mode == 7) {                                              // ColorBurn
        return vec3(cb.r >= 1.0 ? 1.0 : (cs.r <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - cb.r) / cs.r)),
                    cb.g >= 1.0 ? 1.0 : (cs.g <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - cb.g) / cs.g)),
                    cb.b >= 1.0 ? 1.0 : (cs.b <= 0.0 ? 0.0 : 1.0 - min(1.0, (1.0 - cb.b) / cs.b)));
    }
    if (mode == 8) {                                              // HardLight
        return vec3(cs.r <= 0.5 ? 2.0 * cb.r * cs.r : 1.0 - 2.0 * (1.0 - cb.r) * (1.0 - cs.r),
                    cs.g <= 0.5 ? 2.0 * cb.g * cs.g : 1.0 - 2.0 * (1.0 - cb.g) * (1.0 - cs.g),
                    cs.b <= 0.5 ? 2.0 * cb.b * cs.b : 1.0 - 2.0 * (1.0 - cb.b) * (1.0 - cs.b));
    }
    if (mode == 9) {                                              // SoftLight
        vec3 d = vec3(cb.r <= 0.25 ? ((16.0 * cb.r - 12.0) * cb.r + 4.0) * cb.r : sqrt(cb.r),
                      cb.g <= 0.25 ? ((16.0 * cb.g - 12.0) * cb.g + 4.0) * cb.g : sqrt(cb.g),
                      cb.b <= 0.25 ? ((16.0 * cb.b - 12.0) * cb.b + 4.0) * cb.b : sqrt(cb.b));
        return vec3(cs.r <= 0.5 ? cb.r - (1.0 - 2.0 * cs.r) * cb.r * (1.0 - cb.r)
                                 : cb.r + (2.0 * cs.r - 1.0) * (d.r - cb.r),
                    cs.g <= 0.5 ? cb.g - (1.0 - 2.0 * cs.g) * cb.g * (1.0 - cb.g)
                                 : cb.g + (2.0 * cs.g - 1.0) * (d.g - cb.g),
                    cs.b <= 0.5 ? cb.b - (1.0 - 2.0 * cs.b) * cb.b * (1.0 - cb.b)
                                 : cb.b + (2.0 * cs.b - 1.0) * (d.b - cb.b));
    }
    if (mode == 10) return abs(cb - cs);                          // Difference
    if (mode == 11) return cb + cs - 2.0 * cb * cs;                // Exclusion
    if (mode == 12) return setLum(setSat(cs, satOf(cb)), lum(cb)); // Hue
    if (mode == 13) return setLum(setSat(cb, satOf(cs)), lum(cb)); // Saturation
    if (mode == 14) return setLum(cs, lum(cb));                    // Color
    if (mode == 15) return setLum(cb, lum(cs));                    // Luminosity
    if (mode == 16) return min(cb + cs, vec3(1.0));                // Add
    return cs;
}

void main() {
    vec4 src = texture(uSrc, vUV);
    if (uMode == 0) {
        fragColor = src * uOpacity;
        return;
    }
    vec4 dst = texture(uDst, vUV);
    float as = src.a * uOpacity;
    float ab = dst.a;
    vec3 cs = src.a > 1e-5 ? src.rgb / src.a : vec3(0.0);
    vec3 cb = dst.a > 1e-5 ? dst.rgb / dst.a : vec3(0.0);
    vec3 blended = blendChannel(cb, cs, uMode);
    vec3 rgb = (1.0 - ab) * as * cs + ab * as * blended + (1.0 - as) * dst.rgb;
    float outA = as + ab * (1.0 - as);
    fragColor = vec4(rgb, outA);
}
)";

// -----------------------------------------------------------------------------------------------
// docs/tasks/T-54.md: effect-pass fragment shaders (gl_effects.h/.cpp). Every program below is
// linked against `kQuadVert` (the pass's window quad; `vUV` in [0,1], v=0 at the bottom of the
// render target -- see this file's header comment). Buffer
// convention: every RGBA8 input/output texture holds color in xt_gl's existing "Layer" convention
// (see gl_shaders.h's top-of-file comment / gl_renderer.cpp's blend setup): accumulated via
// GL_SRC_ALPHA/GL_ONE_MINUS_SRC_ALPHA blending, so `rgb` is effectively premultiplied by `a`. Most
// passes below (Blur, DropShadow/Glow/Outline's flat-color compositing, GradientMask's uniform
// alpha scaling) are linear in this representation and need no explicit un/re-premultiply step;
// ColorAdjust and Shine divide by `a` to reach straight RGB (docs/design/18-effects.md §18.3's
// formulas are defined in non-premultiplied space) and re-multiply by `a` before writing, per
// docs/tasks/T-54.md's shader-header instruction. Scalar fields (alpha-blur/dilate intermediates)
// are stored in RGBA8 textures' `.r` channel (all four channels written identically so either
// `.r` or `.a` reads work); the signed distance field alone needs range beyond [0,1] and uses a
// dedicated R32F texture (gl_effects.cpp's own, not pooled by FboPool).

inline constexpr const char* kFxGaussScalarFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;      // scalar field, read from .r
uniform vec2 uTexelSize;
uniform vec2 uDir;           // (1,0) horizontal pass, (0,1) vertical pass
uniform int uTapCount;       // merged taps beyond the center sample, 0..32
uniform float uWeights[33];  // index 0 = center weight; 1..uTapCount = merged-pair weights
uniform float uOffsets[33];  // index 0 unused; 1..uTapCount = merged-pair offsets (px)
uniform int uConstEdge;      // 1: taps outside [0,1]^2 read uOutside; 0: GL_CLAMP_TO_EDGE
uniform float uOutside;      // DropShadow inset: the inverted alpha is 1 outside the buffer
float samp(vec2 uv) {
    if (uConstEdge == 1 && (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)) {
        return uOutside;
    }
    return texture(uTex, uv).r;
}
void main() {
    float sum = samp(vUV) * uWeights[0];
    for (int i = 1; i <= 32; ++i) {
        if (i > uTapCount) break;
        vec2 off = uDir * uOffsets[i] * uTexelSize;
        sum += (samp(vUV + off) + samp(vUV - off)) * uWeights[i];
    }
    fragColor = vec4(sum, sum, sum, sum);
}
)";

inline constexpr const char* kFxGaussColorFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform vec2 uDir;
uniform int uTapCount;
uniform float uWeights[33];
uniform float uOffsets[33];
void main() {
    vec4 sum = texture(uTex, vUV) * uWeights[0];
    for (int i = 1; i <= 32; ++i) {
        if (i > uTapCount) break;
        vec2 off = uDir * uOffsets[i] * uTexelSize;
        sum += (texture(uTex, vUV + off) + texture(uTex, vUV - off)) * uWeights[i];
    }
    fragColor = sum;
}
)";

// Separable max filter (DropShadow's `spread`, docs/design/18-effects.md §18.6's "van Herk" note
// -- approximated here as a direct multi-tap max, up to 64 taps per side, accepted per
// docs/tasks/T-54.md's own cost allowance). Out-of-`[0,1]` taps read `uOutside`, normally 0
// (matches xt::sw::fx::maxFilter1D's "samples outside [0,w) treated as 0", unlike the blur passes
// above which rely on GL_CLAMP_TO_EDGE to match xt::sw::fx::boxPass's edge-clamp); DropShadow's
// inset path passes 1, the inverted alpha's value outside the buffer (docs/tasks/T-111.md).
inline constexpr const char* kFxDilateFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform vec2 uDir;
uniform int uRadius;    // 0..64
uniform int uReadAlpha; // 1: read uTex.a (first pass, straight from the color source); 0: read .r
uniform float uOutside; // value of taps outside [0,1]^2 (0 for the outer passes; 1 for the
                        // DropShadow inset path, whose field is the inverted alpha)
void main() {
    float m = 0.0;
    for (int k = -64; k <= 64; ++k) {
        if (k < -uRadius) continue;
        if (k > uRadius) break;
        vec2 uv = vUV + uDir * float(k) * uTexelSize;
        float v = uOutside;
        if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
            v = (uReadAlpha == 1) ? texture(uTex, uv).a : texture(uTex, uv).r;
        }
        m = max(m, v);
    }
    fragColor = vec4(m, m, m, m);
}
)";

// Extracts (optionally inverted) alpha into a scalar field: Glow's `alpha(src)` / `1 - alpha(src)`
// (docs/design/18-effects.md §18.3).
inline constexpr const char* kFxAlphaExtractFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform int uInvert;
void main() {
    float a = texture(uSrc, vUV).a;
    float v = (uInvert == 1) ? (1.0 - a) : a;
    fragColor = vec4(v, v, v, v);
}
)";

// Outline's signed distance field: 8-direction, up-to-64-step nearest-boundary search
// (docs/tasks/T-54.md "多點取樣近似"), `alpha >= 0.5` is foreground (docs/design/18-effects.md
// §18.6). Writes to a dedicated R32F target (gl_effects.cpp); unfound crossings within
// `uSearchRadius` saturate to `uSearchRadius + 1` (Outline's mask formulas already read as 0/1
// once `|sdf| > radius + softness`, so an under-estimated far distance never changes the result).
inline constexpr const char* kFxSdfFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uTexelSize;
uniform float uSearchRadius;
void main() {
    float aC = texture(uSrc, vUV).a;
    bool inside = aC >= 0.5;
    float minDist = uSearchRadius + 1.0;
    vec2 dirs[8];
    dirs[0] = vec2(1.0, 0.0);
    dirs[1] = vec2(-1.0, 0.0);
    dirs[2] = vec2(0.0, 1.0);
    dirs[3] = vec2(0.0, -1.0);
    dirs[4] = vec2(0.70710678, 0.70710678);
    dirs[5] = vec2(-0.70710678, 0.70710678);
    dirs[6] = vec2(0.70710678, -0.70710678);
    dirs[7] = vec2(-0.70710678, -0.70710678);
    int maxSteps = int(ceil(uSearchRadius));
    for (int d = 0; d < 8; ++d) {
        vec2 dir = dirs[d];
        for (int s = 1; s <= 64; ++s) {
            if (s > maxSteps) break;
            vec2 uv = vUV + dir * float(s) * uTexelSize;
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) break;
            float a = texture(uSrc, uv).a;
            bool otherInside = a >= 0.5;
            if (otherInside != inside) {
                minDist = min(minDist, float(s));
                break;
            }
        }
    }
    float sdf = inside ? -minDist : minDist;
    fragColor = vec4(sdf, sdf, sdf, sdf);
}
)";

// ColorAdjust (docs/design/18-effects.md §18.3): `uM`/`uOffset` encode the composed 4x5 matrix
// (row 3 / column 3 always zero -- ColorAdjust's Brightness/Contrast/Saturation/Hue sub-matrices
// never mix in alpha, ColorAdjust's own alpha output instead goes through `uOpacity`).
inline constexpr const char* kFxColorMatrixFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform mat4 uM;
uniform vec4 uOffset;
uniform float uOpacity;
void main() {
    vec4 c = texture(uSrc, vUV);
    vec3 straight = c.a > 1e-5 ? c.rgb / c.a : vec3(0.0);
    vec4 mv = uM * vec4(straight, c.a);
    vec3 outRgb = clamp(mv.rgb + uOffset.rgb, 0.0, 1.0);
    float outA = c.a * uOpacity;
    fragColor = vec4(outRgb * outA, outA);
}
)";

// GradientMask (docs/design/18-effects.md §18.3, gpu_frame.h's GradientMaskParams): `uInvXf` is
// the node's screen->local inverse (device px -> node-local px, with the pass window's origin
// folded in by xt_gpu, T-151); `uP0`/`uP1` are already mapped into local space on the CPU
// (xt_gpu), matching xt::sw::applyGradientMask exactly. `uFbSize` (the window size)
// reconstructs each fragment's window-px position from `vUV` (file comment's y-flip). The stop
// alpha ramp is the 256x1 lookup texture `uLut` (T-147; only .a is used).
inline constexpr const char* kFxGradientMaskFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uLut;
uniform float uLutWidth;
uniform vec2 uFbSize;
uniform mat3 uInvXf;
uniform vec2 uP0;
uniform vec2 uP1;
uniform int uRadial;
uniform int uInvert;

void main() {
    vec2 screenPx = vec2(vUV.x * uFbSize.x, (1.0 - vUV.y) * uFbSize.y);
    vec2 q = (uInvXf * vec3(screenPx, 1.0)).xy;
    vec2 d = uP1 - uP0;
    float lenSq = dot(d, d);
    vec2 v = q - uP0;
    float t;
    if (lenSq <= 0.0) {
        t = 0.0;
    } else if (uRadial == 1) {
        t = length(v) / sqrt(lenSq);
    } else {
        t = dot(v, d) / lenSq;
    }
    t = clamp(t, 0.0, 1.0);
    float u = (t * (uLutWidth - 1.0) + 0.5) / uLutWidth;
    float m = texture(uLut, vec2(u, 0.5)).a;
    if (uInvert == 1) {
        m = 1.0 - m;
    }
    vec4 c = texture(uSrc, vUV);
    fragColor = vec4(c.rgb * m, c.a * m);
}
)";

// Shine (docs/design/18-effects.md §18.3): `uDir`/`uCenter`/`uEdge0`/`uEdge1` are all precomputed
// on the CPU from the content AABB (gl_effects.cpp), matching xt::sw::applyShine exactly.
inline constexpr const char* kFxShineFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform vec2 uFbSize;
uniform vec2 uDir;
uniform float uCenter;
uniform float uEdge0;
uniform float uEdge1;
uniform vec3 uColor;
uniform float uIntensity;

float smoothstepGuarded(float e0, float e1, float x) {
    if (e1 <= e0) {
        return x < e0 ? 0.0 : 1.0;
    }
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    vec2 screenPx = vec2(vUV.x * uFbSize.x, (1.0 - vUV.y) * uFbSize.y);
    float u = screenPx.x * uDir.x + screenPx.y * uDir.y;
    float band = 1.0 - smoothstepGuarded(uEdge0, uEdge1, abs(u - uCenter));
    vec4 c = texture(uSrc, vUV);
    vec3 straight = c.a > 1e-5 ? c.rgb / c.a : vec3(0.0);
    vec3 outRgb = clamp(straight + uColor * band * uIntensity * c.a, 0.0, 1.0);
    fragColor = vec4(outRgb * c.a, c.a);
}
)";

// DropShadow/Glow's final composite step: reads the (already dilated+blurred, or blurred/inverted)
// scalar effect-alpha field `uMaskA`, builds the flat-color effect layer and `srcOver`s it with
// `uSrc` in whichever order `uLineOnTop` says (docs/design/18-effects.md §18.3's DropShadow/Glow
// formulas). `uOffsetUV` is DropShadow's screen-px offset, precomputed into UV space on the CPU
// (0,0 for Glow, which has no offset) -- see gl_effects.cpp for the y-flip-aware conversion.
inline constexpr const char* kFxShadowGlowFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uMaskA;
uniform vec2 uOffsetUV;
uniform float uScale;         // DropShadow: opacity; Glow: intensity
uniform vec3 uColor;
uniform float uColorA;
uniform int uEffectAlphaMul;  // 1: multiply the mask by uSrc's own alpha (Glow inner, DropShadow inset)
uniform int uLineOnTop;       // 1: effect layer is composited on top of uSrc (Glow inner, DropShadow inset)
uniform float uMaskOutside;   // mask value outside [0,1]^2 (1 for DropShadow inset, else 0)
void main() {
    vec2 uv = vUV - uOffsetUV;
    float raw = uMaskOutside;
    if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) {
        raw = texture(uMaskA, uv).r;
    }
    float g = clamp(raw * uScale, 0.0, 1.0);
    vec4 src = texture(uSrc, vUV);
    if (uEffectAlphaMul == 1) {
        g *= src.a;
    }
    float effA = g * uColorA;
    vec3 effPremult = uColor * effA;
    vec4 bottom = (uLineOnTop == 1) ? src : vec4(effPremult, effA);
    vec4 top = (uLineOnTop == 1) ? vec4(effPremult, effA) : src;
    vec3 outRgb = top.rgb + bottom.rgb * (1.0 - top.a);
    float outA = top.a + bottom.a * (1.0 - top.a);
    fragColor = vec4(outRgb, outA);
}
)";

// Outline's final composite step (docs/design/18-effects.md §18.3): reads the SDF (`uSdf`, R32F)
// and applies the outer/center/inner mask formula, then `srcOver`s the resulting line with `uSrc`.
inline constexpr const char* kFxOutlineFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uSdf;
uniform float uRadius;
uniform float uSoftness;
uniform int uPosition;  // 0 outer, 1 center, 2 inner (docs/design/18-effects.md §18.3)
uniform vec3 uColor;
uniform float uColorA;

float smoothstepGuarded(float e0, float e1, float x) {
    if (e1 <= e0) {
        return x < e0 ? 0.0 : 1.0;
    }
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

void main() {
    float sdf = texture(uSdf, vUV).r;
    float m;
    bool lineOnTop;
    if (uPosition == 0) {
        m = sdf > 0.0 ? 1.0 - smoothstepGuarded(uRadius, uRadius + uSoftness, sdf) : 0.0;
        lineOnTop = false;
    } else if (uPosition == 1) {
        m = 1.0 - smoothstepGuarded(uRadius * 0.5, uRadius * 0.5 + uSoftness, abs(sdf));
        lineOnTop = false;
    } else {
        m = sdf <= 0.0 ? 1.0 - smoothstepGuarded(uRadius, uRadius + uSoftness, -sdf) : 0.0;
        lineOnTop = true;
    }
    float lineA = m * uColorA;
    vec3 linePremult = uColor * lineA;
    vec4 src = texture(uSrc, vUV);
    vec4 bottom = lineOnTop ? src : vec4(linePremult, lineA);
    vec4 top = lineOnTop ? vec4(linePremult, lineA) : src;
    vec3 outRgb = top.rgb + bottom.rgb * (1.0 - top.a);
    float outA = top.a + bottom.a * (1.0 - top.a);
    fragColor = vec4(outRgb, outA);
}
)";

// BackdropBlur's final composite step (docs/design/18-effects.md §18.3's kind 8, §18.6): `uSrc` is
// the node's own (already drawn) content, `uBackdrop` the blurred + colour-adjusted copy of what
// lay under the node when PushEffect ran. The blurred backdrop, masked by the content's own alpha,
// goes *under* the content: `dst = srcOver(backdrop * alpha(src), src)`. Both inputs are in this
// file's premultiplied-by-blending convention, and so is the output -- mirrors
// xt::sw::applyBackdropBlur's own composite loop exactly.
inline constexpr const char* kFxBackdropFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uBackdrop;
void main() {
    vec4 src = texture(uSrc, vUV);
    vec4 bottom = texture(uBackdrop, vUV) * src.a;
    float inv = 1.0 - src.a;
    fragColor = vec4(src.rgb + bottom.rgb * inv, src.a + bottom.a * inv);
}
)";

// `EffectProgram::MaskApply` (gpu_frame.h's MaskApplyParams): `uSrc` is the masked content,
// `uMask` the mask shape (both premultiplied); output = src * m. Replaces the pre-T-147 composite
// shader's mask modes 17-20.
inline constexpr const char* kFxMaskApplyFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uMask;
uniform int uMode;  // 0 Alpha, 1 AlphaInverted, 2 Luma, 3 LumaInverted
float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
void main() {
    vec4 mask = texture(uMask, vUV);
    float m;
    if (uMode == 1) {
        m = 1.0 - mask.a;
    } else if (uMode == 2) {
        m = lum(mask.rgb);
    } else if (uMode == 3) {
        m = 1.0 - lum(mask.rgb);
    } else {
        m = mask.a;
    }
    fragColor = texture(uSrc, vUV) * m;
}
)";

// T-343 (docs/design/18-effects.md §18.15, gpu_frame.h `DownsampleParams`): exact 2 x 2 box
// downsample. The fragment's own row is bottom-up in GL, so it is turned top-down, the four
// source texels fetched top-down (clamped to the source's last column / row) and each flipped
// back into GL's row order for the fetch.
inline constexpr const char* kFxDownsampleFrag = R"(#version 330 core
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uSrcSize;
uniform vec2 uDstSize;
vec4 fetchTopDown(ivec2 p) {
    ivec2 srcMax = ivec2(uSrcSize) - ivec2(1);
    p = clamp(p, ivec2(0), srcMax);
    return texelFetch(uTex, ivec2(p.x, srcMax.y - p.y), 0);
}
void main() {
    ivec2 d = ivec2(gl_FragCoord.xy);
    ivec2 top = ivec2(d.x, int(uDstSize.y) - 1 - d.y);
    ivec2 s = top * 2;
    vec4 sum = fetchTopDown(s) + fetchTopDown(s + ivec2(1, 0)) + fetchTopDown(s + ivec2(0, 1)) +
               fetchTopDown(s + ivec2(1, 1));
    fragColor = sum * 0.25;
}
)";

// T-343 (gpu_frame.h `KawaseParams`): one Kawase pass, bilinear and edge-clamped (the pool's
// textures are GL_LINEAR / GL_CLAMP_TO_EDGE). The v axis is flipped in kQuadVert, but the taps
// are symmetric so no sign matters.
inline constexpr const char* kFxKawaseFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uTexelSize;
uniform float uOffset;
void main() {
    vec2 d1 = vec2(uOffset * uTexelSize.x, uOffset * uTexelSize.y);
    vec2 d2 = vec2(uOffset * uTexelSize.x, -uOffset * uTexelSize.y);
    vec4 sum = texture(uTex, vUV) * 4.0;
    sum += texture(uTex, vUV + d1) + texture(uTex, vUV - d1);
    sum += texture(uTex, vUV + d2) + texture(uTex, vUV - d2);
    fragColor = sum * 0.125;
}
)";

// T-343 (gpu_frame.h `LevelMixParams`): bilinear upsample through a uv mapping, mixing two
// levels. `uvM` is computed top-down (gpu_frame.h's convention) and flipped for the sample.
inline constexpr const char* kFxLevelMixFrag = R"(#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uSrc;
uniform sampler2D uBackdrop;
uniform vec2 uScale;
uniform vec2 uOffsetUV;
uniform float uMix;
void main() {
    vec2 top = vec2(vUV.x, 1.0 - vUV.y);
    vec2 m = top * uScale + uOffsetUV;
    vec2 uv = vec2(m.x, 1.0 - m.y);
    vec4 a = texture(uSrc, uv);
    if (uMix > 0.0) {
        vec4 b = texture(uBackdrop, uv);
        a = mix(a, b, uMix);
    }
    fragColor = a;
}
)";

/// Multisample resolve (T-161): a full-screen triangle from `gl_VertexID` (no attributes are
/// read; any VAO may be bound) averaging the `uSamples` samples of a `sampler2DMS` into the
/// single-sample target of the same size, replacing the per-transient `glBlitFramebuffer` whose
/// fixed per-blit cost dominated the MSAA path on both the driver thread and the GPU.
inline constexpr const char* kResolveVert = R"(#version 330 core
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

inline constexpr const char* kResolveFrag = R"(#version 330 core
uniform sampler2DMS uSrc;
uniform int uSamples;
out vec4 fragColor;
void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec4 sum = vec4(0.0);
    for (int i = 0; i < uSamples; ++i) {
        sum += texelFetch(uSrc, c, i);
    }
    fragColor = sum / float(uSamples);
}
)";

}  // namespace xt::gl::shaders
