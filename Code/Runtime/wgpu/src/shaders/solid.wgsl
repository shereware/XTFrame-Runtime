// Shading::Solid and Shading::StencilOnly (color writes off). Mirrors gl_shaders.h kSolidFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    return in.color;
}
