// Shading::ParticleSolid (T-202). Mirrors gl_shaders.h kParticleSolidFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    return in.color;
}
