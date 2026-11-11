// T-660b: Shading::Solid with DrawCmd::coverage (gpu_frame.h): t0 = the R8 coverage page, sampled
// LinearClamp at uv (texel centres, 1:1 on the target's pixels); linear coverage, no gamma.
// Mirrors gl_shaders.h kCoverageSolidFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    return in.color * tex0(in.uv).r;
}
