// EffectProgram::Dilate (gpu_frame.h DilateParams: texel 0-1, dir 2-3, radius 4, readAlpha 5,
// outside 6). Mirrors gl_shaders.h kFxDilateFrag.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let texel = vec2f(pf(0u), pf(1u));
    let dir = vec2f(pf(2u), pf(3u));
    let radius = pi(4u);
    let readAlpha = pi(5u);
    let outside = pf(6u);
    var m = 0.0;
    for (var k = -64; k <= 64; k++) {
        if (k < -radius) {
            continue;
        }
        if (k > radius) {
            break;
        }
        let uv = in.uv + dir * f32(k) * texel;
        var v = outside;
        if (insideUnit(uv)) {
            let s = tex0(uv);
            v = select(s.r, s.a, readAlpha == 1);
        }
        m = max(m, v);
    }
    return vec4f(m, m, m, m);
}
