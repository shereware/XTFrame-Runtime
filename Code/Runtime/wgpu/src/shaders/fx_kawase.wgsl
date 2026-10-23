// EffectProgram::Kawase (T-343; gpu_frame.h KawaseParams: texel 0-1, offset 2). Mirrors gl_shaders.h
// kFxKawaseFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let o = pf(2u);
    let d1 = vec2f(o * pf(0u), o * pf(1u));
    let d2 = vec2f(o * pf(0u), -o * pf(1u));
    var sum = tex0(in.uv) * 4.0;
    sum += tex0(in.uv + d1) + tex0(in.uv - d1);
    sum += tex0(in.uv + d2) + tex0(in.uv - d2);
    return sum * 0.125;
}
