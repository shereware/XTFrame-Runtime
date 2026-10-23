// EffectProgram::LevelMix (T-343; gpu_frame.h LevelMixParams: scale 0-1, offset 2-3, t 4). Mirrors
// gl_shaders.h kFxLevelMixFrag (uvM is top-down here, no flips); t1 = the second level.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let uv = in.uv * vec2f(pf(0u), pf(1u)) + vec2f(pf(2u), pf(3u));
    var a = tex0(uv);
    let t = pf(4u);
    if (t > 0.0) {
        a = mix(a, tex1(uv), t);
    }
    return a;
}
