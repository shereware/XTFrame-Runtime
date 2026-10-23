// Shading::Gradient (gpu_frame.h GradientParams: kind, p0, p1, rx, ry, alpha at pf(0..7)). Mirrors
// gl_shaders.h kGradientFrag; t0 = the 256x1 straight-color LUT, always sampled linearly.
@fragment
fn fs_main(in: VsOut) -> @location(0) vec4f {
    let kind = pi(0u);
    let p0 = vec2f(pf(1u), pf(2u));
    let p1 = vec2f(pf(3u), pf(4u));
    let radialAxes = vec2f(pf(5u), pf(6u));
    let paintAlpha = pf(7u);
    let lutWidth = 256.0;
    let d = p1 - p0;
    let lenSq = dot(d, d);
    var t: f32;
    if (lenSq <= 0.0) {
        t = 1.0;
    } else {
        let v = in.uv - p0;
        if (kind == 4) {
            // T-201: conic -- the clockwise angle from d to v (y down) over a full turn.
            t = atan2(d.x * v.y - d.y * v.x, dot(d, v)) * 0.15915494309;
            t = select(t, t + 1.0, t < 0.0);
        } else {
            t = select(dot(v, d) / lenSq, length(v / radialAxes), kind == 3);
        }
        t = clamp(t, 0.0, 1.0);
    }
    let u = (t * (lutWidth - 1.0) + 0.5) / lutWidth;
    let c = tex0(vec2f(u, 0.5));
    let a = c.a * paintAlpha * in.color.a;
    return vec4f(c.rgb * a, a);
}
