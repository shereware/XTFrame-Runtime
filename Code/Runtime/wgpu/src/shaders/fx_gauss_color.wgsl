// EffectProgram::GaussColor (gpu_frame.h GaussParams, see fx_gauss_scalar.wgsl). Mirrors gl_shaders.h
// kFxGaussColorFrag; t0 = the color source.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let texel = vec2f(pf(0u), pf(1u));
    let dir = vec2f(pf(2u), pf(3u));
    let tapCount = pi(4u);
    var sum = tex0(in.uv) * pf(7u);
    for (var i = 1; i <= 32; i++) {
        if (i > tapCount) {
            break;
        }
        let off = dir * pf(40u + u32(i)) * texel;
        sum += (tex0(in.uv + off) + tex0(in.uv - off)) * pf(7u + u32(i));
    }
    return sum;
}
