// T-668 / T-683 (Docs/design/50-c-abi.md §50.ad item 1): Shading::BoxShadow with one shape -- the pipeline of
// every DrawCmd whose `boxShadowMulti` is 0 (BoxShadowParams::extraCount 0, nearly every shadow). The output
// is the premultiplied colour at pf(12..15) times the coverage of box_shadow_common.wgsl (prepended) at the
// local position in.uv. Mirrors gl_shaders.h kBoxShadowFrag.

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let coverage = bsCoverage(0u, in.uv, pf(8u));
    return vec4f(pf(12u), pf(13u), pf(14u), pf(15u)) * coverage;
}
