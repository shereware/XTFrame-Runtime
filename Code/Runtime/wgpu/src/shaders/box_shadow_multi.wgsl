// T-680 / T-683 (Docs/design/50-c-abi.md §50.ad item 1): Shading::BoxShadow with up to three signed shapes
// (an outlined shadow) -- the pipeline of every DrawCmd whose `boxShadowMulti` is 1. coverage =
// clamp(S(shape 0) + sum of pf(10 + t) x S(shape at pf(16 + 8 t)), 0, 1) for t below pf(9) (at most 2), the
// terms added in that order; output = the premultiplied colour at pf(12..15) times it. S is
// box_shadow_common.wgsl's bsCoverage (prepended). Mirrors gl_shaders.h kBoxShadowMultiFrag.

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let p = in.uv;
    let sigma = pf(8u);
    var coverage = bsCoverage(0u, p, sigma);
    let extra = min(u32(pf(9u) + 0.5), 2u);
    for (var t = 0u; t < extra; t = t + 1u) {
        coverage = coverage + pf(10u + t) * bsCoverage(16u + 8u * t, p, sigma);
    }
    coverage = clamp(coverage, 0.0, 1.0);
    return vec4f(pf(12u), pf(13u), pf(14u), pf(15u)) * coverage;
}
