// EffectProgram::GaussScalar (gpu_frame.h GaussParams: texel 0-1, dir 2-3, tapCount 4, constEdge 5,
// outside 6, weights 7-39, offsets 40-72). Mirrors gl_shaders.h kFxGaussScalarFrag; t0 = the scalar
// field (.r).
fn samp(uv: vec2f) -> f32 {
    if (pi(5u) == 1 && !insideUnit(uv)) {
        return pf(6u);
    }
    return tex0(uv).r;
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let texel = vec2f(pf(0u), pf(1u));
    let dir = vec2f(pf(2u), pf(3u));
    let tapCount = pi(4u);
    var sum = samp(in.uv) * pf(7u);
    for (var i = 1; i <= 32; i++) {
        if (i > tapCount) {
            break;
        }
        let off = dir * pf(40u + u32(i)) * texel;
        sum += (samp(in.uv + off) + samp(in.uv - off)) * pf(7u + u32(i));
    }
    return vec4f(sum, sum, sum, sum);
}
