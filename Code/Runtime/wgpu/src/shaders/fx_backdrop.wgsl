// EffectProgram::Backdrop (no params). Mirrors gl_shaders.h kFxBackdropFrag; t1 = the blurred
// backdrop.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let src = tex0(in.uv);
    let bottom = tex1(in.uv) * src.a;
    let inv = 1.0 - src.a;
    return vec4f(src.rgb + bottom.rgb * inv, src.a + bottom.a * inv);
}
