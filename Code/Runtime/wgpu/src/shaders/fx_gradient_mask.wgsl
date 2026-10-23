// EffectProgram::GradientMask (gpu_frame.h GradientMaskParams: invXf 0-5, p0 6-7, p1 8-9, radial 10,
// invert 11, target size 12-13). Mirrors gl_shaders.h kFxGradientMaskFrag (top-down window px: no
// row flip); t2 = the stop LUT (.a).
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let lutWidth = 256.0;
    let screenPx = vec2f(in.uv.x * pf(12u), in.uv.y * pf(13u));
    let q = vec2f(pf(0u) * screenPx.x + pf(2u) * screenPx.y + pf(4u),
                  pf(1u) * screenPx.x + pf(3u) * screenPx.y + pf(5u));
    let p0 = vec2f(pf(6u), pf(7u));
    let p1 = vec2f(pf(8u), pf(9u));
    let d = p1 - p0;
    let lenSq = dot(d, d);
    let v = q - p0;
    var t: f32;
    if (lenSq <= 0.0) {
        t = 0.0;
    } else if (pi(10u) == 1) {
        t = length(v) / sqrt(lenSq);
    } else {
        t = dot(v, d) / lenSq;
    }
    t = clamp(t, 0.0, 1.0);
    let u = (t * (lutWidth - 1.0) + 0.5) / lutWidth;
    var m = textureSampleLevel(t2, smp, vec2f(u, 0.5), 0.0).a;
    if (pi(11u) == 1) {
        m = 1.0 - m;
    }
    let c = tex0(in.uv);
    return vec4f(c.rgb * m, c.a * m);
}
