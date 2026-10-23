// EffectProgram::ColorMatrix (gpu_frame.h ColorMatrixParams: m 0-8 row-major, offset 9-11,
// opacity 12). Mirrors gl_shaders.h kFxColorMatrixFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let c = tex0(in.uv);
    let s = select(vec3f(0.0), c.rgb / c.a, c.a > 1e-5);
    let mv = vec3f(pf(0u) * s.r + pf(1u) * s.g + pf(2u) * s.b,
                   pf(3u) * s.r + pf(4u) * s.g + pf(5u) * s.b,
                   pf(6u) * s.r + pf(7u) * s.g + pf(8u) * s.b);
    let outRgb = clamp(mv + vec3f(pf(9u), pf(10u), pf(11u)), vec3f(0.0), vec3f(1.0));
    let outA = c.a * pf(12u);
    return vec4f(outRgb * outA, outA);
}
