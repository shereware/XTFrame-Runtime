// EffectProgram::ShadowGlow (gpu_frame.h ShadowGlowParams: offset 0-1, scale 2, rgba 3-6,
// effectAlphaMul 7, lineOnTop 8, maskOutside 9). Mirrors gl_shaders.h kFxShadowGlowFrag; t1 = the
// scalar mask field. The offset is y-down like this uv (GL negates it for its flipped v).
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let uv = in.uv - vec2f(pf(0u), pf(1u));
    var raw = pf(9u);
    if (insideUnit(uv)) {
        raw = tex1(uv).r;
    }
    var g = clamp(raw * pf(2u), 0.0, 1.0);
    let src = tex0(in.uv);
    if (pi(7u) == 1) {
        g *= src.a;
    }
    let effA = g * pf(6u);
    let eff = vec4f(vec3f(pf(3u), pf(4u), pf(5u)) * effA, effA);
    let onTop = pi(8u) == 1;
    let bottom = select(eff, src, onTop);
    let top = select(src, eff, onTop);
    return vec4f(top.rgb + bottom.rgb * (1.0 - top.a), top.a + bottom.a * (1.0 - top.a));
}
