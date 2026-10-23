// Shading::ParticleTexture (T-202; ParticleParams::straight at pf(6)). Mirrors gl_shaders.h
// kParticleTextureFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    var s = tex0(in.uv);
    if (pf(6u) != 0.0) {
        s = vec4f(s.rgb * s.a, s.a);
    }
    return s * in.color;
}
