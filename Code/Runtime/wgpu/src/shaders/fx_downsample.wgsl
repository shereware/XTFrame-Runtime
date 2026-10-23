// EffectProgram::Downsample (T-343; gpu_frame.h DownsampleParams: src size 0-1, dst size 2-3).
// Mirrors gl_shaders.h kFxDownsampleFrag; the fragment row is already top-down here.
fn fetchTopDown(pIn: vec2i) -> vec4f {
    let srcMax = vec2i(vec2f(pf(0u), pf(1u))) - vec2i(1);
    let p = clamp(pIn, vec2i(0), srcMax);
    return textureLoad(t0, p, 0);
}

@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let s = vec2i(floor(in.pos.xy)) * 2;
    let sum = fetchTopDown(s) + fetchTopDown(s + vec2i(1, 0)) + fetchTopDown(s + vec2i(0, 1)) +
              fetchTopDown(s + vec2i(1, 1));
    return sum * 0.25;
}
