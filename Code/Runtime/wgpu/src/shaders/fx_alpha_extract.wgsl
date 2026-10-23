// EffectProgram::AlphaExtract (gpu_frame.h AlphaExtractParams: invert 0). Mirrors gl_shaders.h
// kFxAlphaExtractFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let a = tex0(in.uv).a;
    let v = select(a, 1.0 - a, pi(0u) == 1);
    return vec4f(v, v, v, v);
}
