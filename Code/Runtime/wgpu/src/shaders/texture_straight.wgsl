// Shading::TextureStraight (T-150). Mirrors gl_shaders.h kTextureStraightFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let s = tex0(in.uv);
    return vec4f(s.rgb * s.a, s.a) * in.color;
}
